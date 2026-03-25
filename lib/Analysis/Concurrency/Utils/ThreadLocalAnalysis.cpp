/**
 * @file ThreadLocalAnalysis.cpp
 * @brief Implementation of Thread-Local Storage Detection
 */

#include "Analysis/Concurrency/Utils/ThreadLocalAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <deque>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace ThreadLocal;

namespace {

void collectLocalCarrierLoads(const Value *slot_base,
                              std::vector<const Value *> &loads) {
  if (!slot_base) {
    return;
  }

  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  worklist.push_back(slot_base->stripPointerCasts());
  visited.insert(slot_base->stripPointerCasts());

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();

    for (const User *user : current->users()) {
      const Value *derived = dyn_cast<Value>(user);
      if (!derived || !visited.insert(derived).second) {
        continue;
      }

      if (const auto *load = dyn_cast<LoadInst>(user)) {
        if (load->getPointerOperand()->stripPointerCasts() == current) {
          loads.push_back(load);
        }
        continue;
      }

      if (isa<BitCastInst>(user) || isa<GetElementPtrInst>(user) ||
          isa<PHINode>(user) || isa<SelectInst>(user)) {
        worklist.push_back(derived);
      }
    }
  }
}

} // namespace

ThreadLocalAnalysis::ThreadLocalAnalysis(Module &module) : m_module(module) {}

void ThreadLocalAnalysis::analyze() {
  m_tls_globals.clear();
  m_tls_allocas.clear();
  m_tls_values.clear();
  m_pthread_keys.clear();
  identifyThreadLocalGlobals();
  identifyThreadLocalAllocas();
  identifyPthreadSpecificData();
  
  errs() << "Thread-Local Analysis: Found " << m_tls_globals.size() 
         << " TLS globals, " << m_tls_allocas.size() << " TLS allocas\n";
}

void ThreadLocalAnalysis::identifyThreadLocalGlobals() {
  for (GlobalVariable &gv : m_module.globals()) {
    if (hasThreadLocalStorageLinkage(&gv)) {
      m_tls_globals.insert(&gv);
      m_tls_values.insert(&gv);
    }
  }
}

void ThreadLocalAnalysis::identifyThreadLocalAllocas() {
  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (AllocaInst *alloca = dyn_cast<AllocaInst>(&inst)) {
          // Check if this alloca is thread-local (doesn't escape)
          if (isAllocaThreadLocal(alloca)) {
            m_tls_allocas.insert(alloca);
            m_tls_values.insert(alloca);
          }
        }
      }
    }
  }
}

void ThreadLocalAnalysis::identifyPthreadSpecificData() {
  // pthread_key_create creates thread-specific data keys
  // pthread_getspecific/pthread_setspecific access thread-specific data
  
  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }
    
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallBase *call = dyn_cast<CallBase>(&inst)) {
          Function *callee = call->getCalledFunction();
          if (!callee) {
            callee = dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
          }
          if (!callee) {
            continue;
          }
          
          StringRef name = callee->getName();
          
          // pthread_key_create(pthread_key_t *key, ...)
          if (name.equals("pthread_key_create")) {
            if (call->arg_size() > 0) {
              m_pthread_keys.insert(call->getArgOperand(0));
            }
          }
          
          // pthread_getspecific returns thread-local data
          // pthread_setspecific stores thread-local data
          if (name.equals("pthread_getspecific") ||
              name.equals("pthread_setspecific")) {
            m_tls_values.insert(&inst);
          }
        }
      }
    }
  }
}

bool ThreadLocalAnalysis::hasThreadLocalStorageLinkage(const GlobalVariable *gv) {
  // Check if the global has TLS storage
  return gv->isThreadLocal();
}

bool ThreadLocalAnalysis::isAllocaThreadLocal(const AllocaInst *alloca) const {
  // An alloca is thread-local if:
  // 1. It's a stack allocation (by definition in one thread's stack)
  // 2. Its address doesn't escape to other threads
  //
  // For now, we use a simple heuristic: if the alloca's address is never
  // stored to memory or passed to functions that could share it, it's thread-local
  
  return !escapesThread(alloca);
}

bool ThreadLocalAnalysis::escapesThread(const Value *val) const {
  // Check if a value escapes its thread
  // A value escapes if:
  // - Its address is stored to a global variable
  // - It's passed to a function that could share it (pthread_create, etc.)
  // - It's stored to heap memory that could be accessed by other threads
  
  std::deque<const Value *> worklist;
  std::unordered_set<const Value *> visited;
  
  worklist.push_back(val);
  visited.insert(val);
  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();
  
  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    
    for (const Use &use : current->uses()) {
      const User *user = use.getUser();
      
      // Check for escape scenarios
      if (const StoreInst *store = dyn_cast<StoreInst>(user)) {
        const Value *ptr = store->getPointerOperand();
        const Value *base = getUnderlyingObject(ptr);
        base = base ? base->stripPointerCasts() : nullptr;

        if (store->getValueOperand() == current) {
          const Value *slot_base = base ? base : ptr->stripPointerCasts();
          if (isa<AllocaInst>(slot_base)) {
            std::vector<const Value *> forwarded_loads;
            collectLocalCarrierLoads(slot_base, forwarded_loads);
            for (const Value *load : forwarded_loads) {
              if (visited.insert(load).second) {
                worklist.push_back(load);
              }
            }
            continue;
          }
        }
        
        // Storing to a global = escape
        if (base && isa<GlobalVariable>(base)) {
          return true;
        }
        
        // Storing to heap memory = potential escape
        // (We'd need more sophisticated analysis to determine if heap is shared)
        // For now, conservatively assume heap storage escapes
        if (base) {
          if (!isa<AllocaInst>(base)) {
            return true;
          }
        } else if (!isa<AllocaInst>(ptr)) {
          return true;
        }
      }
      else if (const CallBase *call = dyn_cast<CallBase>(user)) {
        const unsigned op_no = use.getOperandNo();
        const bool is_call_arg = op_no < call->arg_size();
        const bool no_capture = is_call_arg && call->doesNotCapture(op_no);

        // Passing an address as thread payload is a direct cross-thread escape.
        if (thread_api && thread_api->isTDFork(call)) {
          for (const Value *payload : thread_api->getForkPayloadArgs(call)) {
            if (payload == current ||
                (payload && payload->stripPointerCasts() ==
                                current->stripPointerCasts())) {
              return true;
            }
          }
          continue;
        }

        const Function *callee = thread_api ? thread_api->getCallee(call) : nullptr;
        if (!callee) {
          // Unresolved call target (e.g., function pointer/virtual dispatch):
          // unless we can prove nocapture, conservatively treat as escaping.
          if (is_call_arg && !no_capture) {
            return true;
          }
          continue;
        }

        if (!callee->isDeclaration()) {
          if (is_call_arg && op_no < callee->arg_size()) {
            const Argument *formal = callee->getArg(op_no);
            if (formal && visited.insert(formal).second) {
              worklist.push_back(formal);
            }
          }
          continue;
        }

        StringRef name = callee->getName();

        // Known thread/task creation functions = escape
        if (name.contains("pthread_create") ||
            name.contains("std::thread") ||
            name.contains("std::async")) {
          return true;
        }

        // Unknown external functions = potential escape (conservative), unless
        // the specific argument is marked nocapture.
        if (callee->isDeclaration() && !callee->isIntrinsic()) {
          if (!name.startswith("llvm.") &&
              !name.equals("malloc") &&
              !name.equals("free")) {
            if (is_call_arg && no_capture) {
              continue;
            }
            return true;
          }
        }
      }
      else if (const ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
        const Function *parent = ret->getFunction();
        if (!parent) {
          return true;
        }

        bool propagated_to_caller = false;
        for (const User *function_user : parent->users()) {
          const auto *cb = dyn_cast<CallBase>(function_user);
          if (!cb) {
            continue;
          }
          if (thread_api && thread_api->getCallee(cb) != parent) {
            continue;
          }
          propagated_to_caller = true;
          if (visited.insert(cb).second) {
            worklist.push_back(cb);
          }
        }

        if (!propagated_to_caller && !parent->hasLocalLinkage()) {
          return true;
        }
      }
      else if (const Instruction *inst = dyn_cast<Instruction>(user)) {
        // Continue tracking through casts, GEPs, etc.
        if (isa<BitCastInst>(inst) || isa<GetElementPtrInst>(inst)) {
          if (visited.insert(inst).second) {
            worklist.push_back(inst);
          }
        }
      }
    }
  }
  
  return false; // Doesn't escape
}

bool ThreadLocalAnalysis::isThreadLocal(const Value *val) const {
  // Direct check
  if (m_tls_values.count(val)) {
    return true;
  }
  
  // Check if it's a global with TLS
  if (const GlobalVariable *gv = dyn_cast<GlobalVariable>(val)) {
    return m_tls_globals.count(gv) > 0;
  }
  
  // Check if it's a thread-local alloca
  if (const AllocaInst *alloca = dyn_cast<AllocaInst>(val)) {
    return m_tls_allocas.count(alloca) > 0;
  }
  
  // Trace through GEP, bitcast, etc.
  if (const GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(val)) {
    return isThreadLocal(gep->getPointerOperand());
  }
  
  if (const BitCastInst *cast = dyn_cast<BitCastInst>(val)) {
    return isThreadLocal(cast->getOperand(0));
  }

  // Soundness fix:
  // A load from thread-local storage does NOT imply the loaded pointee/value is
  // itself thread-local. TLS slots frequently store pointers to shared/global
  // state.  Treating the loaded SSA value as thread-local can suppress real
  // races on the referenced object.
  if (const LoadInst *load = dyn_cast<LoadInst>(val)) {
    if (m_tls_values.count(load)) {
      return true;
    }
    return false;
  }
  
  return false;
}

bool ThreadLocalAnalysis::accessesThreadLocalStorage(const Instruction *inst) const {
  auto accessesKnownThreadLocalBase = [this](const Value *ptr) {
    if (!ptr) {
      return false;
    }
    ptr = ptr->stripPointerCasts();
    if (const Value *base = getUnderlyingObject(ptr)) {
      ptr = base->stripPointerCasts();
    }
    if (const auto *gv = dyn_cast<GlobalVariable>(ptr)) {
      return m_tls_globals.count(gv) > 0;
    }
    if (const auto *alloca = dyn_cast<AllocaInst>(ptr)) {
      return m_tls_allocas.count(alloca) > 0;
    }
    return false;
  };

  // Only accesses to known thread-local storage bases are pruned here.
  // Values returned by pthread_getspecific may themselves point to shared
  // memory, so deriving an address from that result is not enough to prove
  // the dereference thread-local.
  if (const LoadInst *load = dyn_cast<LoadInst>(inst)) {
    return accessesKnownThreadLocalBase(load->getPointerOperand());
  }
  
  if (const StoreInst *store = dyn_cast<StoreInst>(inst)) {
    return accessesKnownThreadLocalBase(store->getPointerOperand());
  }
  
  if (const AtomicRMWInst *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    return accessesKnownThreadLocalBase(rmw->getPointerOperand());
  }
  
  if (const AtomicCmpXchgInst *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
    return accessesKnownThreadLocalBase(cmpxchg->getPointerOperand());
  }
  
  return false;
}

bool ThreadLocal::isObviouslyThreadLocal(const Value *val) {
  // Fast check without full analysis
  
  // Global with TLS
  if (const GlobalVariable *gv = dyn_cast<GlobalVariable>(val)) {
    return gv->isThreadLocal();
  }
  
  // Stack allocations are thread-local by default
  // (though they can escape, this is a fast heuristic)
  if (isa<AllocaInst>(val)) {
    return true;
  }
  
  return false;
}
