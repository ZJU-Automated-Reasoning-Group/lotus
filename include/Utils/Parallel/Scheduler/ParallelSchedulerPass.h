#ifndef LLVMUTILS_SCHEDULER_PARALLELSCHEDULERPASS_H
#define LLVMUTILS_SCHEDULER_PARALLELSCHEDULERPASS_H

#include <functional>
#include <string>

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

using namespace llvm;

/**
 * ParallelSchedulerPass is an example LLVM ModulePass that demonstrates
 * how to use the PipelineScheduler for parallel analysis.
 *
 * Usage example:
 *   auto *Pass = new ParallelSchedulerPass();
 *   Pass->setAnalysisCallback([](const Function *F) {
 *     // Your analysis code here
 *   });
 *   Pass->runOnModule(M);
 */
class ParallelSchedulerPass : public ModulePass {
private:
  /// Client-provided analysis callback
  std::function<void(const Function *)> AnalysisCallback;

  /// Client-provided garbage collection callback
  std::function<void(const Function *)> GCCallback;

  /// Human-readable name for this pass
  std::string Name;

  /// Analysis type (local, bottom-up, or top-down)
  int AnalysisType; // 0=Local, 1=BottomUp, 2=TopDown

  /// Enable garbage collection
  bool EnableGC;

public:
  static char ID;

  ParallelSchedulerPass();
  virtual ~ParallelSchedulerPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  bool runOnModule(Module &M) override;

public:
  /// Set the human-readable name for this scheduler pass
  void setName(const char *N) { this->Name.assign(N); }

  /// Set the analysis callback that will be invoked for each function
  void setAnalysisCallback(std::function<void(const Function *)> CB) {
    AnalysisCallback = CB;
  }

  /// Set the garbage collection callback for memory cleanup
  void setGCCallback(std::function<void(const Function *)> CB) {
    GCCallback = CB;
  }

  /// Set analysis type: 0=Local, 1=BottomUp, 2=TopDown
  void setAnalysisType(int Type) { AnalysisType = Type; }

  /// Enable/disable automatic garbage collection
  void setEnableGC(bool Enable) { EnableGC = Enable; }
};

#endif // LLVMUTILS_SCHEDULER_PARALLELSCHEDULERPASS_H
