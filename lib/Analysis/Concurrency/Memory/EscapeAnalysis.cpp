#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <deque>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

namespace {

const Function *resolveInternalCallee(const CallBase *call) {
  if (!call) {
    return nullptr;
  }
  if (Function *direct = call->getCalledFunction()) {
    return direct;
  }
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

} // namespace

EscapeAnalysis::EscapeAnalysis(Module &module) : m_module(module) {}

void EscapeAnalysis::analyze() {
  m_escaped_values.clear();
  runEscapeAnalysis();
}

bool EscapeAnalysis::isEscaped(const Value *val) const {
  if (!val)
    return false;
  
  if (isa<GlobalValue>(val))
    return true;

  const Value *stripped = val->stripPointerCasts();
  if (m_escaped_values.count(stripped)) {
    return true;
  }

  const Value *obj = getUnderlyingObject(stripped);
  if (obj && obj != stripped && m_escaped_values.count(obj->stripPointerCasts())) {
    return true;
  }

  return false;
}

bool EscapeAnalysis::isThreadLocal(const Value *val) const {
  return !isEscaped(val);
}

void EscapeAnalysis::runEscapeAnalysis() {
  std::deque<const Value *> worklist;
  auto enqueueEscaped = [&](const Value *value) {
    if (!value) return;
    const Value *stripped = value->stripPointerCasts();
    if (m_escaped_values.insert(stripped).second) {
      worklist.push_back(stripped);
    }
    const Value *obj = getUnderlyingObject(stripped);
    if (obj && obj != stripped) {
      const Value *objStripped = obj->stripPointerCasts();
      if (m_escaped_values.insert(objStripped).second) {
        worklist.push_back(objStripped);
      }
    }
  };

  // 1. Initial seeds
  for (const GlobalValue &gv : m_module.globals()) {
    enqueueEscaped(&gv);
  }

  auto *threadAPI = ThreadAPI::getThreadAPI();
  for (Function &F : m_module) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      Instruction *inst = &*I;
      if (threadAPI->isTDFork(inst)) {
        for (const Value *arg : threadAPI->getForkPayloadArgs(inst)) {
          if (arg && arg->getType()->isPointerTy()) {
            enqueueEscaped(arg);
          }
        }
      }
    }
  }

  // 2. Fixed-point propagation (inclusion-based)
  while (!worklist.empty()) {
    const Value *curr = worklist.front();
    worklist.pop_front();

    for (const User *U : curr->users()) {
      if (const auto *inst = dyn_cast<Instruction>(U)) {
        if (const auto *store = dyn_cast<StoreInst>(inst)) {
          if (store->getValueOperand() == curr) {
            // value escapes to pointer
            enqueueEscaped(store->getPointerOperand());
          } else if (store->getPointerOperand() == curr) {
            // pointer is escaped, so any value stored in it escapes
            enqueueEscaped(store->getValueOperand());
          }
        } else if (const auto *load = dyn_cast<LoadInst>(inst)) {
          // loading from escaped pointer makes result escaped
          enqueueEscaped(load);
        } else if (isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst) ||
                   isa<PHINode>(inst) || isa<SelectInst>(inst) || isa<AddrSpaceCastInst>(inst)) {
          enqueueEscaped(inst);
        } else if (const auto *cb = dyn_cast<CallBase>(inst)) {
          // Argument passing
          const Function *callee = resolveInternalCallee(cb);
          if (callee && !callee->isDeclaration()) {
            for (unsigned i = 0; i < cb->arg_size(); ++i) {
              if (cb->getArgOperand(i) == curr && i < callee->arg_size()) {
                enqueueEscaped(callee->getArg(i));
              }
            }
          }
          // If the call site itself is escaped (e.g. return value escapes), 
          // then the return values from all returns in callee escape.
          if (cb == curr && callee && !callee->isDeclaration()) {
             for (const BasicBlock &BB : *callee) {
               if (const auto *ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
                 if (ret->getReturnValue()) enqueueEscaped(ret->getReturnValue());
               }
             }
          }
        } else if (const auto *ret = dyn_cast<ReturnInst>(inst)) {
          // Return value escapes to all call sites
          const Function *F = ret->getFunction();
          for (const User *FU : F->users()) {
            if (const auto *cb = dyn_cast<CallBase>(FU)) {
              if (resolveInternalCallee(cb) == F) enqueueEscaped(cb);
            }
          }
        }
      }
    }

    // Handle Argument -> CallSite (reverse of above)
    if (const auto *arg = dyn_cast<Argument>(curr)) {
      const Function *F = arg->getParent();
      for (const User *U : F->users()) {
        if (const auto *cb = dyn_cast<CallBase>(U)) {
          if (resolveInternalCallee(cb) == F && arg->getArgNo() < cb->arg_size()) {
            enqueueEscaped(cb->getArgOperand(arg->getArgNo()));
          }
        }
      }
    }
  }
}

} // namespace lotus
