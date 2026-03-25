#include "Optimization/SWPrefetching/SWPrefetchingInternal.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace llvm {

using namespace sampleprof;

bool SWPrefetchingLLVMPass::runOnFunction(Function &F) {
  bool modified = false;
  LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  const bool wantProfile =
      PrefetchDistanceProviderMode == PrefetchDistanceProvider::Profile;
  const bool wantLBR =
      PrefetchDistanceProviderMode == PrefetchDistanceProvider::LBR;
  const bool wantLLM =
      PrefetchDistanceProviderMode == PrefetchDistanceProvider::LLM;
  const bool wantStatic =
      PrefetchDistanceProviderMode == PrefetchDistanceProvider::StaticAnalysis;

  if (wantProfile && !Reader) {
    return false;
  }
  bool samplesExist = false;
  const FunctionSamples *SamplesReaded = nullptr;
  if (Reader) {
    SamplesReaded = Reader->getSamplesFor(F);
    if (SamplesReaded) {
      samplesExist = true;
    }
  }

  if (wantProfile && samplesExist) {
    int64_t prefechDist;
    SmallVector<Instruction *, 30> AllCurLoads;
    SmallVector<int64_t, 30> AllPrefetchDist;
    SmallVector<int64_t, 20> correctMapping;
    std::vector<SmallVector<Instruction *, 20>> AllCapturedInstrs;
    std::vector<SmallVector<Instruction *, 10>> AllCapturedPhis;
    std::vector<SmallVector<Instruction *, 10>> AllCapturedLoads;

    for (auto &BB : F) {
      bool isBBLoop = LI.getLoopFor(&BB);
      if (isBBLoop) {
        for (auto &I : BB) {
          const ErrorOr<Hints> T = getHints(I, SamplesReaded);
          if (T) {
            if (LoadInst *curLoad = dyn_cast<LoadInst>(&I)) {
              for (const auto &S_V : *T) {
                prefechDist = static_cast<int64_t>(S_V.second);
                Instruction *phi = nullptr;
                SmallVector<Instruction *, 10> Loads;
                SmallVector<Instruction *, 20> Instrs;
                SmallVector<Instruction *, 10> Phis;

                if (SearchAlgorithm(curLoad, LI, phi, Loads, Instrs, Phis)) {
                  for (size_t index = 0; index < Phis.size(); index++) {
                    Instrs.push_back(Phis[Phis.size() - 1 - index]);
                  }
                  AllCurLoads.push_back(curLoad);
                  AllPrefetchDist.push_back(prefechDist);
                  AllCapturedInstrs.push_back(Instrs);
                  AllCapturedPhis.push_back(Phis);
                  AllCapturedLoads.push_back(Loads);
                }
              }
            }
          }
        }
      }
    }

    bool correctMappingCheck = false;
    SmallVector<Instruction *, 10> AlreadyPrefetched;

    if (AllCurLoads.size() > 1) {
      for (size_t i = 0; i < AllCurLoads.size(); i++) {
        for (size_t j = 0; j < AllCurLoads.size(); j++) {
          if (AllCapturedInstrs[i].size() == AllCapturedInstrs[j].size() &&
              AllCurLoads[i] != AllCurLoads[j]) {
            if (AllCapturedLoads[i].size() == AllCapturedLoads[j].size() &&
                AllCapturedPhis[i].size() == AllCapturedPhis[j].size()) {
              if (!(std::find(correctMapping.begin(), correctMapping.end(), i) !=
                    correctMapping.end())) {
                correctMapping.push_back(i);
                correctMappingCheck = true;
              }
            }
          }
        }
      }
    }
    if (correctMappingCheck) {
      for (size_t j = 0; j < AllCurLoads.size(); j++) {
        if (!(std::find(correctMapping.begin(), correctMapping.end(), j) !=
              correctMapping.end())) {
          if (!(std::find(AlreadyPrefetched.begin(), AlreadyPrefetched.end(),
                          AllCurLoads[j]) != AlreadyPrefetched.end())) {
            AlreadyPrefetched.push_back(AllCurLoads[j]);
            if (AllCapturedPhis[j].size() > 1) {
              if (InjectPrefeches(AllCurLoads[j], LI, AllCapturedPhis[j],
                                  AllCapturedLoads[j], AllCapturedInstrs[j],
                                  AllPrefetchDist[j], true)) {
                modified = true;
              }
            } else if (AllCapturedPhis[j].size() == 1 &&
                       AllCapturedLoads[j].size() != 0) {
              if (InjectPrefechesOnePhiPartOne(
                      AllCurLoads[j], LI, AllCapturedPhis[j],
                      AllCapturedLoads[j], AllCapturedInstrs[j],
                      AllPrefetchDist[j], true)) {
                modified = true;
              }
            }
          }
        }
      }
    }
    if (!correctMappingCheck) {
      for (size_t j = 0; j < AllCurLoads.size(); j++) {
        if (!(std::find(AlreadyPrefetched.begin(), AlreadyPrefetched.end(),
                        AllCurLoads[j]) != AlreadyPrefetched.end())) {
          AlreadyPrefetched.push_back(AllCurLoads[j]);
          if (AllCapturedPhis[j].size() > 1) {
            if (InjectPrefeches(AllCurLoads[j], LI, AllCapturedPhis[j],
                                AllCapturedLoads[j], AllCapturedInstrs[j],
                                AllPrefetchDist[j], true)) {
              modified = true;
            }
          } else if (AllCapturedPhis[j].size() == 1) {
            if (InjectPrefechesOnePhiPartOne(
                    AllCurLoads[j], LI, AllCapturedPhis[j], AllCapturedLoads[j],
                    AllCapturedInstrs[j], AllPrefetchDist[j], true)) {
              modified = true;
            }
          }
        }
      }
    }
  }

  auto parseDistanceList = [&](const cl::list<std::string> &Distances,
                               int64_t &Out) -> bool {
    if (Distances.empty()) {
      return false;
    }
    for (auto &e : Distances) {
      Out = std::stoull(e);
    }
    return true;
  };

  auto runStaticDistancePrefetch = [&](int64_t pd) {
    SmallVector<Instruction *, 10> AllLoadsDepToPhix;
    std::vector<SmallVector<Instruction *, 20>> AllDependentInstsx;
    std::vector<SmallVector<Instruction *, 10>> AllDependentPhisx;
    SmallVector<Instruction *, 10> StrideLoadsx;
    SmallVector<Instruction *, 10> StrideLoadsToKeepx;
    SmallVector<Instruction *, 10> IndirectLoadsx;
    SmallVector<Instruction *, 10> IndirectLoadsToKeepx;
    SmallVector<Instruction *, 10> LoadsToRemovex;
    SmallVector<int, 10> LoadsIndexx;

    std::vector<SmallVector<Instruction *, 20>>
        AllDependentInstrsToIndirectLoadx;
    std::vector<SmallVector<Instruction *, 20>> AllDependentInstrsToStrideLoadx;
    std::vector<SmallVector<Instruction *, 10>> AllDependentPhisToStrideLoadx;
    std::vector<SmallVector<Instruction *, 10>> AllDependentPhisToIndirectLoadx;

    for (auto &BB : F) {
      bool isBBLoop = LI.getLoopFor(&BB);
      for (auto &I : BB) {
        if (isBBLoop) {
          if (LoadInst *curLoad = dyn_cast<LoadInst>(&I)) {
            Instruction *phi = nullptr;
            SmallVector<Instruction *, 10> DependentLoadsToCurLoadx;
            SmallVector<Instruction *, 20> DependentInstrsToCurLoadx;
            SmallVector<Instruction *, 10> DependentPhisx;
            if (IsDep(curLoad, LI, phi, DependentLoadsToCurLoadx,
                      DependentInstrsToCurLoadx, DependentPhisx)) {
              if (DependentLoadsToCurLoadx.size() > 0) {
                int indexOfDepLoad = 0;
                bool DepPhiOfDepLoad = false;
                for (auto &s : DependentLoadsToCurLoadx) {
                  for (size_t i = 0; i < AllLoadsDepToPhix.size(); i++) {
                    if (AllLoadsDepToPhix[i] == s) {
                      DepPhiOfDepLoad = true;
                      indexOfDepLoad = i;
                    }
                  }
                  if (DepPhiOfDepLoad) {
                    bool foundall = false;
                    for (auto &d : AllDependentInstsx[indexOfDepLoad]) {
                      for (auto &sd : DependentInstrsToCurLoadx) {
                        if (d == sd) {
                          foundall = true;
                        }
                      }
                      if (!foundall) {
                        continue;
                      }
                    }
                    if (foundall) {
                      SmallVector<Instruction *, 20>
                          DependentInstrsToIndirectLoadx;
                      SmallVector<Instruction *, 20>
                          DependentInstrsToStrideLoadx;
                      SmallVector<Instruction *, 10>
                          DependentPhistoIndirectLoadx;
                      SmallVector<Instruction *, 10> DependentPhistoStrideLoadx;

                      IndirectLoadsx.push_back(curLoad);
                      StrideLoadsx.push_back(s);
                      for (auto &si : DependentInstrsToCurLoadx) {
                        DependentInstrsToIndirectLoadx.push_back(si);
                      }
                      for (auto &di : AllDependentInstsx[indexOfDepLoad]) {
                        DependentInstrsToStrideLoadx.push_back(di);
                      }
                      for (auto &si : DependentPhisx) {
                        DependentPhistoIndirectLoadx.push_back(si);
                      }
                      for (auto &di : AllDependentPhisx[indexOfDepLoad]) {
                        DependentPhistoStrideLoadx.push_back(di);
                      }
                      AllDependentInstrsToIndirectLoadx.push_back(
                          DependentInstrsToIndirectLoadx);
                      AllDependentInstrsToStrideLoadx.push_back(
                          DependentInstrsToStrideLoadx);
                      AllDependentPhisToIndirectLoadx.push_back(
                          DependentPhistoIndirectLoadx);
                      AllDependentPhisToStrideLoadx.push_back(
                          DependentPhistoStrideLoadx);
                    }
                    DepPhiOfDepLoad = false;
                  }
                }
              }
              AllLoadsDepToPhix.push_back(curLoad);
              AllDependentInstsx.push_back(DependentInstrsToCurLoadx);
              AllDependentPhisx.push_back(DependentPhisx);
            }
          }
        }
      }
    }

    for (size_t x = 0; x < StrideLoadsx.size(); x++) {
      for (size_t y = 0; y < IndirectLoadsx.size(); y++) {
        if (StrideLoadsx[x] == IndirectLoadsx[y]) {
          if (AllDependentPhisToStrideLoadx[x] ==
              AllDependentPhisToIndirectLoadx[y]) {
            LoadsToRemovex.push_back(StrideLoadsx[x]);
          }
        }
      }
    }

    for (size_t x = 0; x < StrideLoadsx.size(); x++) {
      bool kept = false;
      if (LoadsToRemovex.size() > 0) {
        for (size_t y = 0; y < LoadsToRemovex.size(); y++) {
          if (StrideLoadsx[x] != LoadsToRemovex[y] &&
              IndirectLoadsx[x] != LoadsToRemovex[y]) {
            kept = true;
          }
        }
        if (kept) {
          StrideLoadsToKeepx.push_back(StrideLoadsx[x]);
          LoadsIndexx.push_back(x);
          IndirectLoadsToKeepx.push_back(IndirectLoadsx[x]);
        }
      } else {
        StrideLoadsToKeepx.push_back(StrideLoadsx[x]);
        LoadsIndexx.push_back(x);
        IndirectLoadsToKeepx.push_back(IndirectLoadsx[x]);
      }
    }
    for (size_t x = 0; x < IndirectLoadsToKeepx.size(); x++) {
      if (InjectPrefechesOnePhiPartTwo(
              IndirectLoadsToKeepx[x], LI,
              AllDependentPhisToIndirectLoadx[LoadsIndexx[x]][0],
              AllDependentInstrsToIndirectLoadx[LoadsIndexx[x]], pd)) {
        modified = true;
      }
      if (InjectPrefechesOnePhiPartTwo(
              StrideLoadsToKeepx[x], LI,
              AllDependentPhisToStrideLoadx[LoadsIndexx[x]][0],
              AllDependentInstrsToStrideLoadx[LoadsIndexx[x]], pd * 2)) {
        modified = true;
      }
    }
  };

  if (wantProfile) {
    if (!AutoFDOMapping) {
      int64_t pd = 0;
      if (parseDistanceList(LBR_dist, pd)) {
        runStaticDistancePrefetch(pd);
      }
    }
  } else if (wantLBR) {
    int64_t pd = 0;
    if (parseDistanceList(LBR_dist, pd)) {
      runStaticDistancePrefetch(pd);
    }
  } else if (wantLLM) {
    int64_t pd = 0;
    if (parseDistanceList(LLM_dist, pd)) {
      runStaticDistancePrefetch(pd);
    }
  } else if (wantStatic) {
    // Reserved for static-analysis driven distances.
  }
  return modified;
}

} // namespace llvm
