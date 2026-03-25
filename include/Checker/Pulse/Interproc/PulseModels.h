#ifndef CHECKER_PULSE_PULSEMODELS_H
#define CHECKER_PULSE_PULSEMODELS_H

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

class PulseChecker;

/**
 * ModelResult: Result of applying a model
 * - handled: true if the model handled the call
 * - states: list of resulting execution states
 */
struct ModelResult {
  bool handled;
  std::vector<ExecutionDomain> states;

  static ModelResult notHandled() { return {false, {}}; }
  static ModelResult success(std::vector<ExecutionDomain> s) {
    return {true, std::move(s)};
  }
};

/**
 * PulseModel: A function type that models a specific function call
 */
using PulseModel = std::function<ModelResult(
    PulseChecker &checker, const llvm::CallInst *call, ExecutionDomain &state,
    const llvm::BasicBlock *pred)>;

/**
 * PulseModels: Manages library models and taint configurations
 */
class PulseModels {
public:
  PulseModels(PulseChecker &checker);

  /**
   * Try to dispatch a call to a registered model
   */
  ModelResult dispatch(const llvm::CallInst *call, ExecutionDomain &state,
                       const llvm::BasicBlock *pred);

  /**
   * Check if a function has a model registered
   */
  bool hasModel(const llvm::Function *func) const;

  /**
   * Check if a function name is a taint source
   */
  bool isTaintSource(const std::string &func_name) const;

  /**
   * Check if a function name is a taint sink
   */
  bool isTaintSink(const std::string &func_name) const;

  /**
   * Check if a function name is a taint sanitizer
   */
  bool isTaintSanitizer(const std::string &func_name) const;

private:
  PulseChecker &checker_;
  PulseOperations &ops_;
  AbstractValueFactory &factory_;

  // Registry of function models
  std::map<std::string, PulseModel> models_;

  // Taint configurations
  struct TaintConfig {
    std::set<std::string> sources;
    std::set<std::string> sinks;
    std::set<std::string> propagators;
    std::set<std::string> sanitizers;

    bool isSource(const std::string &name) const {
      return sources.find(name) != sources.end();
    }

    bool isSink(const std::string &name) const {
      return sinks.find(name) != sinks.end();
    }

    bool isSanitizer(const std::string &name) const {
      return sanitizers.find(name) != sanitizers.end();
    }
  } taint_config_;

  void registerStandardModels();
  void registerTaintModels();
  void registerCppModels();

  // Model implementations
  ModelResult modelMalloc(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelCalloc(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelFree(const llvm::CallInst *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelRealloc(const llvm::CallInst *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred);
  ModelResult modelFileOpen(const llvm::CallInst *call, ExecutionDomain &state,
                            const llvm::BasicBlock *pred);
  ModelResult modelFileClose(const llvm::CallInst *call, ExecutionDomain &state,
                             const llvm::BasicBlock *pred);
  ModelResult modelLock(const llvm::CallInst *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelUnlock(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStringCopy(const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred);
  ModelResult modelStrdup(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrchr(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrstr(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelMemcpy(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelMemmove(const llvm::CallInst *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred);
  ModelResult modelMemset(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelRead(const llvm::CallInst *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelWrite(const llvm::CallInst *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelFread(const llvm::CallInst *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelFwrite(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelFgets(const llvm::CallInst *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelGets(const llvm::CallInst *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);

  // C++ Models
  ModelResult modelStdVectorPushBack(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdVectorAccess(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdVectorData(const llvm::CallInst *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdStringCStr(const llvm::CallInst *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);

  // std::unique_ptr
  ModelResult modelStdUniquePtrRelease(const llvm::CallInst *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrReset(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrGet(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrDtor(const llvm::CallInst *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);

  // POSIX Models
  ModelResult modelSocket(const llvm::CallInst *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);

  // Explicit escape sinks (sound incorrectness): functions that store a
  // pointer argument for later use beyond the current stack frame.
  ModelResult modelPthreadCreate(const llvm::CallInst *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelThrdCreate(const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred);
  ModelResult modelDispatchAsyncF(const llvm::CallInst *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred);

  // Taint models
  ModelResult modelTaintSource(const llvm::CallInst *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred,
                               const std::string &kind);
  ModelResult modelTaintSink(const llvm::CallInst *call, ExecutionDomain &state,
                             const llvm::BasicBlock *pred,
                             const std::string &kind);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEMODELS_H
