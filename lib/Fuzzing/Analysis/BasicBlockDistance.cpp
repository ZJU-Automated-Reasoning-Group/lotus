// CCS 17: Directed Greybox Fuzzing (AFLGo)
//
// This file implements basic block distance analysis for directed fuzzing.
// It computes the control-flow distance from each basic block to target
// locations, used to guide fuzzing towards specific program points.

#include "Fuzzing/Analysis/BasicBlockDistance.h"
#include "Fuzzing/Analysis/ExtendedCallGraph.h"
#include "Fuzzing/Analysis/FunctionDistance.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/ADT/BreadthFirstIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

// Scale factor for inter-procedural distances to prioritize intra-procedural paths.
const double FunctionDistanceMagnificationFactor = 10;

AnalysisKey AFLGoBasicBlockDistanceAnalysis::Key;

// Identifies origin basic blocks (targets and call sites to functions with targets)
// and associates them with initial distances based on call graph proximity.
AFLGoBasicBlockDistanceAnalysis::Result
AFLGoBasicBlockDistanceAnalysis::run(Module &M, ModuleAnalysisManager &MAM) {
  AFLGoBasicBlockDistanceAnalysis::Result::FunctionToOriginBBsMapTy
      FunctionToOriginBBs;

  CallGraph *CG = nullptr;
  if (!UseExtendedCG) {
    CG = &MAM.getResult<CallGraphAnalysis>(M);
  } else {
    CG = &MAM.getResult<ExtendedCallGraphAnalysis>(M);
  }

  auto &FunctionDistances = MAM.getResult<AFLGoFunctionDistanceAnalysis>(M);
  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    SmallDenseMap<BasicBlock *, double, 16> OriginBBs;

    auto &Targets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
    for (auto &TargetBBPair : Targets.BBs) {
      auto *TargetBB = TargetBBPair.first;
      OriginBBs[TargetBB] = 0;
    }

    auto *CGNode = (*CG)[&F];
    for (auto &CallEdge : *CGNode) {
      auto &CallInstOpt = CallEdge.first;
      if (!CallInstOpt) {
        continue;
      }
      CallBase *CallInst = cast<CallBase>(*CallInstOpt);

      auto *CalleeNode = CallEdge.second;
      auto *CalledFunction = CalleeNode->getFunction();
      if (FunctionDistances.find(CalledFunction) == FunctionDistances.end()) {
        continue;
      }
      auto CallBBDistance = (FunctionDistances[CalledFunction] + 1) *
                            FunctionDistanceMagnificationFactor;

      auto *CallBB = CallInst->getParent();
      if (OriginBBs.find(CallBB) != OriginBBs.end()) {
        // When multiple calls appear in the same basic block, keep the one that
        // generates the minimum distance.
        OriginBBs[CallBB] = std::min(OriginBBs[CallBB], CallBBDistance);
      } else {
        OriginBBs[CallBB] = CallBBDistance;
      }
    }

    FunctionToOriginBBs[&F] = OriginBBs;
  }

  return Result{FunctionToOriginBBs, FunctionDistances};
}

// Computes distance from each basic block to the nearest target using backward BFS
// from origin blocks. Uses harmonic mean for blocks reachable from multiple origins.
AFLGoBasicBlockDistanceAnalysis::Result::BBToDistanceTy
AFLGoBasicBlockDistanceAnalysis::Result::computeBBDistances(Function &F) {
  auto OriginBBs = FunctionToOriginBBs[&F];

  auto DistanceMap = AFLGoBasicBlockDistanceAnalysis::Result::BBToDistanceTy();
  std::map<BasicBlock *, std::vector<double>> DistancesFromOrigins;
  for (auto &OriginBBPair : OriginBBs) {
    auto *OriginBB = OriginBBPair.first;
    auto OriginBBDistance = OriginBBPair.second;
    DistanceMap[OriginBB] = OriginBBDistance;

    auto InverseOriginBB = static_cast<Inverse<BasicBlock *>>(OriginBB);
    for (auto BFIter = bf_begin(InverseOriginBB);
         BFIter != bf_end(InverseOriginBB); ++BFIter) {
      if (OriginBBs.find(*BFIter) != OriginBBs.end()) {
        // This basic block is either a target or performs an external call.
        continue;
      }

      DistancesFromOrigins[*BFIter].push_back(OriginBBDistance +
                                              BFIter.getLevel());
    }
  }

  for (auto &DistancesFromOriginPair : DistancesFromOrigins) {
    auto &Distances = DistancesFromOriginPair.second;

    double HarmonicMean = 0;
    for (auto Distance : Distances) {
      HarmonicMean += 1.0 / Distance;
    }
    HarmonicMean = Distances.size() / HarmonicMean;

    auto *BB = DistancesFromOriginPair.first;
    DistanceMap[BB] = HarmonicMean;
  }

  return DistanceMap;
}