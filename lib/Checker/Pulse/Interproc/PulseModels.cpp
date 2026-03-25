#include "Checker/Pulse/Interproc/PulseModels.h"

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <limits>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace pulse {

namespace {
static llvm::Optional<uint64_t> getConstUInt64(const llvm::Value *v) {
  if (!v)
    return llvm::None;
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    if (CI->getBitWidth() <= 64) {
      if (CI->isNegative())
        return llvm::None;
      return CI->getZExtValue();
    }
  }
  return llvm::None;
}

static llvm::Optional<uint64_t> mulConstU64(uint64_t a, uint64_t b) {
  __int128 prod = static_cast<__int128>(a) * static_cast<__int128>(b);
  if (prod < 0 || prod > std::numeric_limits<uint64_t>::max()) {
    return llvm::None;
  }
  return static_cast<uint64_t>(prod);
}

static void reportOutOfBoundsAccess(const llvm::Instruction *loc,
                                    const Address &addr, const char *detail) {
  Trace trace = Trace::fromValueHistory(addr.history);
  trace.addEvent(loc, detail);
  auto diag = std::make_unique<AccessToInvalidAddress>(
      loc, "Out of bounds access", detail,
      "Ensure indices and lengths stay within the allocated object",
      IssueType::OutOfBounds, std::move(trace));
  DiagnosticManager::getInstance().report(std::move(diag));
}

static void checkLengthAgainstAllocation(AbductiveDomain &astate,
                                         const Address &addr,
                                         const llvm::Value *len_value,
                                         const llvm::Instruction *loc,
                                         const char *detail) {
  auto len_opt = getConstUInt64(len_value);
  if (!len_opt)
    return;
  AbstractValue canon = astate.getCanonical(addr.addr);
  auto size_opt = astate.getAllocationSize(canon);
  if (!size_opt)
    return;
  if (*len_opt > *size_opt) {
    reportOutOfBoundsAccess(loc, addr, detail);
    astate.getPostAttrs().add(canon, Attribute::OutOfBounds);
  }
}
} // namespace

PulseModels::PulseModels(PulseChecker &checker)
    : checker_(checker), ops_(checker.getOperations()),
      factory_(checker.getFactory()) {
  registerStandardModels();
  registerTaintModels();
  registerCppModels();
}

ModelResult PulseModels::dispatch(const llvm::CallInst *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred) {
  const llvm::Function *func = call->getCalledFunction();
  if (!func || !func->hasName()) {
    return ModelResult::notHandled();
  }

  std::string funcName = func->getName().str();

  // Try exact match first
  auto it = models_.find(funcName);
  if (it != models_.end()) {
    return it->second(checker_, call, state, pred);
  }

  // Try pattern matching for C++ mangled names (e.g., _ZNSt10unique_ptrI*ED1Ev)
  // For now, just check for common C++ operators
  if (funcName == "_ZdlPv" || funcName == "_ZdaPv") {
    // These should have been registered, but check anyway
    if (funcName == "_ZdlPv" || funcName == "_ZdaPv") {
      return modelFree(call, state, pred);
    }
  }

  return ModelResult::notHandled();
}

bool PulseModels::hasModel(const llvm::Function *func) const {
  if (!func || !func->hasName()) {
    return false;
  }
  return models_.find(func->getName().str()) != models_.end();
}

bool PulseModels::isTaintSource(const std::string &func_name) const {
  return taint_config_.isSource(func_name);
}

bool PulseModels::isTaintSink(const std::string &func_name) const {
  return taint_config_.isSink(func_name);
}

bool PulseModels::isTaintSanitizer(const std::string &func_name) const {
  return taint_config_.isSanitizer(func_name);
}

void PulseModels::registerStandardModels() {
  // Memory allocation
  models_["malloc"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred);
  };
  models_["calloc"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelCalloc(call, state, pred);
  };
  models_["free"] = [this](PulseChecker &, const llvm::CallInst *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred);
  };
  models_["realloc"] = [this](PulseChecker &, const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelRealloc(call, state, pred);
  };

  // Thread creation: the context argument escapes to another thread.
  models_["pthread_create"] = [this](PulseChecker &, const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
    return modelPthreadCreate(call, state, pred);
  };
  models_["thrd_create"] = [this](PulseChecker &, const llvm::CallInst *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred) {
    return modelThrdCreate(call, state, pred);
  };

  // libdispatch: context escapes to asynchronous work item.
  models_["dispatch_async_f"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelDispatchAsyncF(call, state, pred);
      };

  // File operations
  models_["fopen"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred);
  };
  models_["fclose"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };
  models_["open"] = [this](PulseChecker &, const llvm::CallInst *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred);
  };
  models_["close"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };
  models_["fdopen"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred);
  };
  models_["opendir"] = [this](PulseChecker &, const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred);
  };
  models_["closedir"] = [this](PulseChecker &, const llvm::CallInst *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };

  // String operations
  models_["strcpy"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStringCopy(call, state, pred);
  };
  models_["strncpy"] = [this](PulseChecker &, const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelStringCopy(call, state, pred);
  };
  models_["strdup"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrdup(call, state, pred);
  };
  models_["strchr"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrchr(call, state, pred);
  };
  models_["strstr"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrstr(call, state, pred);
  };
  models_["memcpy"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMemcpy(call, state, pred);
  };
  models_["memmove"] = [this](PulseChecker &, const llvm::CallInst *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelMemmove(call, state, pred);
  };
  models_["memset"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMemset(call, state, pred);
  };
  models_["bzero"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelMemset(call, state, pred);
  };

  // I/O operations
  models_["read"] = [this](PulseChecker &, const llvm::CallInst *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelRead(call, state, pred);
  };
  models_["write"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelWrite(call, state, pred);
  };
  models_["fread"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFread(call, state, pred);
  };
  models_["fwrite"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFwrite(call, state, pred);
  };
  models_["fgets"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFgets(call, state, pred);
  };
  models_["gets"] = [this](PulseChecker &, const llvm::CallInst *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelGets(call, state, pred);
  };

  // Locking
  models_["pthread_mutex_lock"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelLock(call, state, pred);
      };
  models_["pthread_mutex_unlock"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelUnlock(call, state, pred);
      };
}

namespace {
static void reportStackEscapeOnCall(AbductiveDomain &astate,
                                    const llvm::CallInst *call,
                                    const Address &escaped_addr,
                                    const char *reason) {
  AbstractValue canon = astate.getCanonical(escaped_addr.addr);
  if (!astate.getPostAttrs().has(canon, Attribute::Stack)) {
    return;
  }

  Trace trace = Trace::fromValueHistory(escaped_addr.history);
  trace.addEvent(call, reason);
  auto diag = std::make_unique<StackVariableAddressEscape>(
      call, canon, "Stack address escapes via call",
      "Do not pass addresses of local variables to APIs that store them for "
      "later use.",
      std::move(trace));
  DiagnosticManager::getInstance().report(std::move(diag));
}
} // namespace

ModelResult PulseModels::modelPthreadCreate(const llvm::CallInst *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  // int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
  //                   void *(*start_routine)(void*), void *arg);
  if (call->arg_size() < 4)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(*astate, call, *arg_opt,
                            "Passing stack-derived context to pthread_create "
                            "(escapes to new thread)");
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelThrdCreate(const llvm::CallInst *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  // int thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
  if (call->arg_size() < 3)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(2), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(
        *astate, call, *arg_opt,
        "Passing stack-derived context to thrd_create (escapes to new thread)");
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelDispatchAsyncF(const llvm::CallInst *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  // void dispatch_async_f(dispatch_queue_t queue, void *context,
  // dispatch_function_t work);
  if (call->arg_size() < 3)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto ctx_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (ctx_opt) {
    reportStackEscapeOnCall(*astate, call, *ctx_opt,
                            "Passing stack-derived context to dispatch_async_f "
                            "(escapes asynchronously)");
  }
  return ModelResult::success({state});
}

void PulseModels::registerTaintModels() {
  // Taint sources - functions that introduce taint
  taint_config_.sources.insert("read");
  taint_config_.sources.insert("recv");
  taint_config_.sources.insert("gets");
  taint_config_.sources.insert("fgets");
  taint_config_.sources.insert("scanf");
  taint_config_.sources.insert("fscanf");
  taint_config_.sources.insert("getenv");
  taint_config_.sources.insert("getcwd");
  taint_config_.sources.insert("recvfrom");
  taint_config_.sources.insert("recvmsg");

  // Taint sinks - functions that consume tainted data
  taint_config_.sinks.insert("system");
  taint_config_.sinks.insert("exec");
  taint_config_.sinks.insert("eval");
  taint_config_.sinks.insert("execl");
  taint_config_.sinks.insert("execlp");
  taint_config_.sinks.insert("execle");
  taint_config_.sinks.insert("execv");
  taint_config_.sinks.insert("execvp");
  taint_config_.sinks.insert("execve");
  taint_config_.sinks.insert("popen");
  taint_config_.sinks.insert("fprintf");
  taint_config_.sinks.insert("printf");
  taint_config_.sinks.insert("sprintf");
  taint_config_.sinks.insert("snprintf");

  // Taint sanitizers - functions that remove taint
  taint_config_.sanitizers.insert("strlen");
  taint_config_.sanitizers.insert("strcmp");
  taint_config_.sanitizers.insert("strncmp");
  taint_config_.sanitizers.insert("atoi");
  taint_config_.sanitizers.insert("atol");
  taint_config_.sanitizers.insert("atoll");
  taint_config_.sanitizers.insert("strtol");
  taint_config_.sanitizers.insert("strtoul");
}

void PulseModels::registerCppModels() {
  // C++ new/delete operators
  models_["_Znwm"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred); // operator new
  };
  models_["_Znam"] = [this](PulseChecker &, const llvm::CallInst *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred); // operator new[]
  };
  models_["_ZdlPv"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred); // operator delete
  };
  models_["_ZdaPv"] = [this](PulseChecker &, const llvm::CallInst *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred); // operator delete[]
  };

  // std::vector
  models_["_ZNSt6vectorI*E9push_backERK*"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelStdVectorPushBack(call, state, pred);
      };

  // std::unique_ptr
  models_["_ZNSt10unique_ptrI*E7releaseEv"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelStdUniquePtrRelease(call, state, pred);
      };
  models_["_ZNSt10unique_ptrI*E5resetE*"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelStdUniquePtrReset(call, state, pred);
      };
  models_["_ZNSt10unique_ptrI*E3getEv"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelStdUniquePtrGet(call, state, pred);
      };
  models_["_ZNSt10unique_ptrI*ED1Ev"] =
      [this](PulseChecker &, const llvm::CallInst *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelStdUniquePtrDtor(call, state, pred);
      };
}

// --- Model Implementations ---

ModelResult PulseModels::modelMalloc(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  // Create fresh value for return
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);

  // Mark as allocated
  ops_.allocate(*astate, ret_val, call);

  // Initialize history with allocation event
  ret_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction());

  // Track size if available
  if (call->arg_size() > 0) {
    if (auto size_opt = getConstUInt64(call->getArgOperand(0))) {
      astate->setAllocationSize(ret_val, *size_opt);
    }
  }

  // Return the address
  astate->getPostStack().add(call, ret_addr);

  // malloc can fail: fork state?
  // For now, assume success (common case)
  // To support null case:
  // ExecutionDomain failureState = state.clone();
  // failureState.getAstate()->getPathFormula().addNull(ret_val);
  // ...
  // state.getAstate()->getPathFormula().addNonNull(ret_val);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFree(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  // Evaluate the pointer argument - handle bitcasts properly
  const llvm::Value *arg = call->getArgOperand(0);

  // If it's a bitcast, get the source value
  if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(arg)) {
    arg = BC->getOperand(0);
  } else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(arg)) {
    if (CE->getOpcode() == llvm::Instruction::BitCast) {
      arg = CE->getOperand(0);
    }
  }

  auto ptr_opt = ops_.eval(*astate, arg, call, pred);
  if (!ptr_opt) {
    // Try evaluating the original argument if bitcast eval failed
    ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
    if (!ptr_opt)
      return ModelResult::success({state});
  }

  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);

  // free(NULL) is a no-op (C/C++). Treating it as an invalidation would later
  // produce false UseAfterFree reports due to Invalid taking precedence over
  // NullDereference. For sound incorrectness, skip invalidation only when
  // NULL is proven by the path condition.
  if (astate->getPathFormula().isNull(canon_ptr)) {
    return ModelResult::success({state});
  }

  // Freeing non-heap memory is undefined behavior. Report only when we can
  // prove non-heap provenance (Stack/Global), then stop exploring the path to
  // avoid downstream UB-driven false positives.
  if (astate->getPostAttrs().has(canon_ptr, Attribute::Stack) ||
      astate->getPostAttrs().has(canon_ptr, Attribute::Global)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Invalid free: non-heap pointer");
    std::string msg = "Invalid free of ";
    msg += astate->getPostAttrs().has(canon_ptr, Attribute::Stack) ? "stack"
                                                                   : "global";
    msg += " memory";
    auto diag = std::make_unique<InvalidFree>(
        call, canon_ptr, msg,
        "Only free heap allocations (malloc/new). Do not free stack/global "
        "addresses.",
        std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
    astate->setPathFormula(
        std::make_unique<PulseFormula>(PulseFormula::contradiction()));
    return ModelResult::success({state});
  }

  // Check for double free: if pointer is already invalid, report double free
  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    // Double free detected!
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Double free: freeing already freed pointer");

    // Get the original invalidation info to show where it was first freed
    auto inv_info = astate->getInvalidationInfo(canon_ptr);
    if (inv_info) {
      trace.addEvent(inv_info->second, "First free");
    }

    // Report as UseAfterFree (double free is a form of use-after-free)
    // We could add a specific DoubleFree type, but UseAfterFree works
    auto diag = std::make_unique<AccessToInvalidAddress>(
        call, "Double free detected", "Freeing already freed memory",
        "Ensure each allocation is freed exactly once", IssueType::UseAfterFree,
        std::move(trace), InvalidationKind::CFree);
    DiagnosticManager::getInstance().report(std::move(diag));

    // Still invalidate (though it's already invalid)
    // This ensures the state is consistent
  }

  // Invalidate the canonical pointer value
  ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::CFree);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelRealloc(const llvm::CallInst *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  // realloc(NULL, size) behaves like malloc(size). Skip invalidation only
  // when NULL is proven by the current path condition.
  // IMPORTANT: Don't invalidate the old pointer here! realloc only invalidates
  // the old pointer if it succeeds. If realloc fails (returns NULL), the old
  // pointer remains valid. We'll handle invalidation when we see the assignment
  // p = new_p (which indicates realloc succeeded). This prevents false
  // positives. The invalidation happens in
  // PulseCheckerInstructions.cpp::handleStore() when we see p = new_p.

  // And then behaves like malloc
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call);
  if (auto size_opt = getConstUInt64(call->getArgOperand(1))) {
    astate->setAllocationSize(ret_val, *size_opt);
  }
  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFileOpen(const llvm::CallInst *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);

  // Mark as Resource (File)
  // We reuse allocate/Attribute::Invalid logic but with a different attribute?
  // Or just track it as "Allocated" but with a special kind?
  // For now, treat as allocation
  ops_.allocate(*astate, ret_val, call);

  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFileClose(const llvm::CallInst *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!ptr_opt)
    return ModelResult::success({state});

  ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::FClose);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelLock(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  // Simplified lock model: check if already locked?
  // For now just no-op or state update
  return ModelResult::success({state});
}

ModelResult PulseModels::modelUnlock(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  (void)call;
  (void)pred;
  // Unlocking releases synchronization state; it does not destroy the lock.
  // Reusing memory invalidation here causes spurious use-after-free reports on
  // subsequent accesses to the same lock object.
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStringCopy(const llvm::CallInst *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && src_opt) {
    // Read from src (check validity)
    auto read_res = ops_.readDeref(*astate, *src_opt, call);
    if (read_res.first != OperationResult::Success) {
      // Report error via DiagnosticManager?
      // ops_.readDeref usually handles reporting internally or returns error
      // Here we should probably bubble up the error
    }

    // Write to dest
    // In a real model we would copy the abstract value content or taint
    // For now, just check write validity
    AbstractValue dummy = factory_.createFresh(
        call); // Dummy value representing the string content
    ops_.writeDeref(*astate, *dest_opt, Address(dummy), call);

    // Return dest
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdVectorPushBack(const llvm::CallInst *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  // vector::push_back(val)
  // 1. Check vector validity
  // 2. Potentially invalidate iterators (realloc)
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdVectorAccess(const llvm::CallInst *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  // vector::operator[]
  // Check bounds?
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdVectorData(const llvm::CallInst *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdStringCStr(const llvm::CallInst *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  return ModelResult::success({state});
}

ModelResult PulseModels::modelSocket(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  return modelFileOpen(call, state, pred); // Treat socket like file for now
}

ModelResult PulseModels::modelTaintSource(const llvm::CallInst *call,
                                          ExecutionDomain &state,
                                          const llvm::BasicBlock *pred,
                                          const std::string &kind) {
  // TODO: Mark return value as tainted
  return ModelResult::success({state});
}

ModelResult PulseModels::modelTaintSink(const llvm::CallInst *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred,
                                        const std::string &kind) {
  // TODO: Check if args are tainted
  return ModelResult::success({state});
}

// C++ Smart Pointers (reusing existing implementations in header, just ensuring
// they are linked)
// ... (The header implementation is sufficient if inline, otherwise move here)
// Since I removed them from header to avoid redefinition if I included it, I
// should move them here if they were inline. But wait, the previous `Read`
// showed them as member functions in `PulseModels.h` but implemented in
// `PulseModels.cpp`? No, the previous `Read` of `PulseModels.cpp` (before I
// overwrote it) showed them implemented there. I have preserved them in my new
// `PulseModels.cpp`.

ModelResult
PulseModels::modelStdUniquePtrRelease(const llvm::CallInst *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);

  Address *raw_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);
  Address ret_addr;

  if (raw_ptr) {
    ret_addr = *raw_ptr;
  } else {
    AbstractValue new_ptr = factory_.createFresh(call);
    ret_addr = Address(new_ptr);
  }

  AbstractValue null_val =
      factory_.getOrCreate(llvm::Constant::getNullValue(call->getType()));
  astate->getPostHeap().addEdge(this_av, ptr_access, Address(null_val));
  astate->getPathFormula().addNull(null_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrReset(const llvm::CallInst *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);
  Address *old_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);

  if (old_ptr) {
    ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
  }

  Address new_addr;
  if (call->arg_size() > 1) {
    auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
    if (val_opt)
      new_addr = *val_opt;
    else
      new_addr = Address(factory_.createFresh(call));
  } else {
    AbstractValue null_val = factory_.getOrCreate(llvm::Constant::getNullValue(
        llvm::PointerType::get(call->getContext(), 0)));
    new_addr = Address(null_val);
    astate->getPathFormula().addNull(null_val);
  }

  astate->getPostHeap().addEdge(this_av, ptr_access, new_addr);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrGet(const llvm::CallInst *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);

  Address *raw_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);
  Address ret_addr;

  if (!raw_ptr) {
    AbstractValue new_ptr = factory_.createFresh(call);
    ret_addr = Address(new_ptr);
    astate->getPostHeap().addEdge(this_av, ptr_access, ret_addr);
    astate->abduceToPre(this_av, ptr_access, ret_addr);
  } else {
    ret_addr = *raw_ptr;
  }

  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrDtor(const llvm::CallInst *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);
  Address *old_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);

  if (old_ptr) {
    ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelCalloc(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  // calloc(nmemb, size) allocates nmemb * size bytes, initialized to zero
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);

  // Mark as allocated and initialized
  ops_.allocate(*astate, ret_val, call);
  ops_.initialize(*astate, ret_val);
  if (call->arg_size() >= 2) {
    auto n_opt = getConstUInt64(call->getArgOperand(0));
    auto sz_opt = getConstUInt64(call->getArgOperand(1));
    if (n_opt && sz_opt) {
      if (auto total_opt = mulConstU64(*n_opt, *sz_opt)) {
        astate->setAllocationSize(ret_val, *total_opt);
      }
    }
  }

  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrdup(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto src_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!src_opt)
    return ModelResult::success({state});

  // Check source validity
  auto read_res = ops_.readDeref(*astate, *src_opt, call);
  if (read_res.first != OperationResult::Success) {
    return ModelResult::success({state}); // Error already reported
  }

  // Allocate new string (like malloc)
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call);
  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrchr(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!str_opt)
    return ModelResult::success({state});

  // Check string validity
  auto read_res = ops_.readDeref(*astate, *str_opt, call);
  if (read_res.first != OperationResult::Success) {
    return ModelResult::success({state});
  }

  // Return null or pointer into string (non-deterministic)
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrstr(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto haystack_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto needle_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (haystack_opt) {
    auto read_res = ops_.readDeref(*astate, *haystack_opt, call);
    if (read_res.first != OperationResult::Success) {
      return ModelResult::success({state});
    }
  }
  if (needle_opt) {
    auto read_res = ops_.readDeref(*astate, *needle_opt, call);
    if (read_res.first != OperationResult::Success) {
      return ModelResult::success({state});
    }
  }

  // Return null or pointer into haystack (non-deterministic)
  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelMemcpy(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && src_opt) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "memcpy writes beyond destination buffer");
    checkLengthAgainstAllocation(*astate, *src_opt, call->getArgOperand(2),
                                 call, "memcpy reads beyond source buffer");
    // Check validity
    auto src_read = ops_.readDeref(*astate, *src_opt, call);
    if (src_read.first == OperationResult::Success) {
      // Write to dest (simplified - real implementation would copy content)
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *dest_opt, Address(dummy), call);
      astate->getPostStack().add(call, *dest_opt);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelMemmove(const llvm::CallInst *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  // For safety, treat memmove like memcpy for now (overlap not modeled).
  return modelMemcpy(call, state, pred);
}

ModelResult PulseModels::modelMemset(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto value_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && value_opt) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "memset writes beyond destination buffer");
    // Write value to dest (simplified - real implementation would write to all
    // bytes)
    ops_.writeDeref(*astate, *dest_opt, *value_opt, call);
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelRead(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto buf_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (buf_opt) {
    checkLengthAgainstAllocation(*astate, *buf_opt, call->getArgOperand(2),
                                 call, "read writes beyond destination buffer");
    // Read into buffer (simplified)
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *buf_opt, Address(dummy), call);

    // Return bytes read (non-deterministic)
    AbstractValue ret_val = factory_.createFresh(call);
    Address ret_addr(ret_val);
    astate->getPostStack().add(call, ret_addr);
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelWrite(const llvm::CallInst *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto buf_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (buf_opt) {
    checkLengthAgainstAllocation(*astate, *buf_opt, call->getArgOperand(2),
                                 call, "write reads beyond source buffer");
    // Check buffer validity
    auto read_res = ops_.readDeref(*astate, *buf_opt, call);
    if (read_res.first == OperationResult::Success) {
      // Return bytes written (non-deterministic)
      AbstractValue ret_val = factory_.createFresh(call);
      Address ret_addr(ret_val);
      astate->getPostStack().add(call, ret_addr);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFread(const llvm::CallInst *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (stream_opt && ptr_opt) {
    auto size_opt = getConstUInt64(call->getArgOperand(1));
    auto count_opt = getConstUInt64(call->getArgOperand(2));
    if (size_opt && count_opt) {
      if (auto total_opt = mulConstU64(*size_opt, *count_opt)) {
        AbstractValue canon = astate->getCanonical(ptr_opt->addr);
        auto alloc_size_opt = astate->getAllocationSize(canon);
        if (alloc_size_opt && *total_opt > *alloc_size_opt) {
          reportOutOfBoundsAccess(call, *ptr_opt,
                                  "fread writes beyond destination buffer");
          astate->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      }
    }
    // Check stream validity
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    if (stream_read.first == OperationResult::Success) {
      // Write to ptr
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *ptr_opt, Address(dummy), call);

      // Return items read
      AbstractValue ret_val = factory_.createFresh(call);
      Address ret_addr(ret_val);
      astate->getPostStack().add(call, ret_addr);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFwrite(const llvm::CallInst *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (stream_opt && ptr_opt) {
    auto size_opt = getConstUInt64(call->getArgOperand(1));
    auto count_opt = getConstUInt64(call->getArgOperand(2));
    if (size_opt && count_opt) {
      if (auto total_opt = mulConstU64(*size_opt, *count_opt)) {
        AbstractValue canon = astate->getCanonical(ptr_opt->addr);
        auto alloc_size_opt = astate->getAllocationSize(canon);
        if (alloc_size_opt && *total_opt > *alloc_size_opt) {
          reportOutOfBoundsAccess(call, *ptr_opt,
                                  "fwrite reads beyond source buffer");
          astate->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      }
    }
    // Check stream and ptr validity
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    auto ptr_read = ops_.readDeref(*astate, *ptr_opt, call);
    if (stream_read.first == OperationResult::Success &&
        ptr_read.first == OperationResult::Success) {
      // Return items written
      AbstractValue ret_val = factory_.createFresh(call);
      Address ret_addr(ret_val);
      astate->getPostStack().add(call, ret_addr);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFgets(const llvm::CallInst *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(2), call, pred);

  if (str_opt && stream_opt) {
    checkLengthAgainstAllocation(*astate, *str_opt, call->getArgOperand(1),
                                 call,
                                 "fgets writes beyond destination buffer");
    // Check stream validity
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    if (stream_read.first == OperationResult::Success) {
      // Write to str
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *str_opt, Address(dummy), call);

      // Return str or null
      astate->getPostStack().add(call, *str_opt);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelGets(const llvm::CallInst *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (str_opt) {
    // Write to str (unsafe - no bounds checking)
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *str_opt, Address(dummy), call);

    // Return str or null
    astate->getPostStack().add(call, *str_opt);
  }

  return ModelResult::success({state});
}

} // namespace pulse
