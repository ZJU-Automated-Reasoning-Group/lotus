#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/PassManager.h>

namespace llvm {

class ExtendedCallGraphAnalysis
    : public AnalysisInfoMixin<ExtendedCallGraphAnalysis> {
public:
  static AnalysisKey Key;

  using Result = llvm::CallGraph;

  Result run(Module &M, ModuleAnalysisManager &);
};

} // namespace llvm