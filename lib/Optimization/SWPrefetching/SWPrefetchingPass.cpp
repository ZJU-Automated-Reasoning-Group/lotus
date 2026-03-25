// from https://github.com/SabaJamilan/Profile-Guided-Software-Prefetching
// EuroSys 22: APT-GET: profile-guided timely software prefetching

#include "Optimization/SWPrefetching/SWPrefetchingInternal.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/ProfileData/SampleProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

using namespace sampleprof;

bool AutoFDOMapping;

cl::opt<PrefetchDistanceProvider> PrefetchDistanceProviderMode(
    "prefetch-distance-provider", cl::desc("Select prefetch distance provider"),
    cl::values(clEnumValN(PrefetchDistanceProvider::Profile, "profile",
                          "Use sample profile hints (default)"),
               clEnumValN(PrefetchDistanceProvider::LBR, "lbr",
                          "Use user-provided LBR distances"),
               clEnumValN(PrefetchDistanceProvider::LLM, "llm",
                          "Use LLM-provided distances"),
               clEnumValN(PrefetchDistanceProvider::StaticAnalysis, "static",
                          "Use static-analysis distances (reserved)")),
    cl::init(PrefetchDistanceProvider::Profile));

cl::opt<std::string> PrefetchFile(
    "input-file", cl::desc("Specify input filename for mypass"),
    cl::value_desc("filename"));

cl::list<std::string> LBR_dist("dist",
                               cl::desc("Specify offset value from LBR"),
                               cl::Hidden, cl::ZeroOrMore);
cl::list<std::string> LLM_dist("llm-dist",
                               cl::desc("Specify offset value from LLM"),
                               cl::Hidden, cl::ZeroOrMore);

ErrorOr<Hints> getHints(const Instruction &Inst,
                        const sampleprof::FunctionSamples *TopSamples) {
  if (const auto &Loc = Inst.getDebugLoc()) {
    if (const auto *Samples = TopSamples->findFunctionSamples(Loc)) {
      return Samples->findCallTargetMapAt(FunctionSamples::getOffset(Loc),
                                          Loc->getBaseDiscriminator());
    }
  }
  return std::error_code();
}

char SWPrefetchingLLVMPass::ID = 0;

void SWPrefetchingLLVMPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesCFG();
}

bool SWPrefetchingLLVMPass::doInitialization(Module &M) {
  if (PrefetchDistanceProviderMode == PrefetchDistanceProvider::Profile) {
    if (PrefetchFile.empty()) {
      errs() << "PrefetchFile is Empty!\n";
      return false;
    }

    LLVMContext &Ctx = M.getContext();
    const std::string &PrefetchFilePath = PrefetchFile.getValue();
    ErrorOr<std::unique_ptr<SampleProfileReader>> ReaderOrErr =
        SampleProfileReader::create(PrefetchFilePath, Ctx);
    if (std::error_code EC = ReaderOrErr.getError()) {
      std::string Msg = "Could not open profile: " + EC.message();
      Ctx.diagnose(DiagnosticInfoSampleProfile(
          PrefetchFilePath, Msg, DiagnosticSeverity::DS_Warning));
      return false;
    }

    Reader = std::move(ReaderOrErr.get());
    Reader->read();

    for (auto &F : M) {
      const sampleprof::FunctionSamples *SamplesReaded = Reader->getSamplesFor(F);
      if (SamplesReaded) {
        AutoFDOMapping = true;
      }
    }

    return true;
  }

  AutoFDOMapping = false;
  return true;
}

static RegisterPass<SWPrefetchingLLVMPass>
    X("SWPrefetchingLLVMPass", "Hello SWPrefetchingLLVMPass", true, true);

} // namespace llvm
