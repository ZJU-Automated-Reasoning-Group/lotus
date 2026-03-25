/*
FIXME: this MemorySSA is tighly bounded to DSA alias analysis, which is not a
good idea.
*/
#include "IR/MemorySSA/MemorySSA.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace previrt {
namespace analysis {

using namespace llvm;

MemorySSACallSite::MemorySSACallSite(CallInst *ci, bool only_singleton)
    : m_ci(ci), m_only_singleton(only_singleton) {
  // Traverse backwards up to the beginning of the block searching
  // for shadow.mem.XXX functions.
  bool first = true;
  auto it = m_ci->getReverseIterator();
  ++it;
  for (auto et = m_ci->getParent()->rend(); it != et; ++it) {
    CallBase *CB = dyn_cast<CallBase>(&*it);
    if (!CB) {
      break;
    }

    // XXX: we store all actual parameters regardless only_singleton flag
    if (CB->getCalledFunction() &&
        (isMemSSAArgRef(CB, false /*m_only_singleton*/) ||
         isMemSSAArgMod(CB, false /*m_only_singleton*/) ||
         isMemSSAArgRefMod(CB, false /*m_only_singleton*/) ||
         isMemSSAArgNew(CB, false /*m_only_singleton*/))) {
      // get "index" field from the callsite
      int64_t idx = getMemSSAParamIdx(CB);
      if (idx < 0) {
        report_fatal_error("[IP-DSE] cannot find index in shadow.mem function");
      }
      if (first) {
        m_actual_params.resize(idx + 1);
        first = false;
      }
      m_actual_params[idx] = CB;
    } else {
      // no more shadow.mem functions so that we can stop here
      break;
    }
  }
}

// return true if the shadow.mem.XXX instruction associated with the
// idx-th actual paramter is shadow.mem.arg_ref.
bool MemorySSACallSite::isRef(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  return isMemSSAArgRef(m_actual_params[idx], m_only_singleton);
}

// return true if the shadow.mem.XXX instruction associated with the
// idx-th actual paramter is shadow.mem.arg_mod.
bool MemorySSACallSite::isMod(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  return isMemSSAArgMod(m_actual_params[idx], m_only_singleton);
}

// return true if the shadow.mem.XXX instruction associated with the
// idx-th actual paramter is shadow.mem.arg_ref_mod.
bool MemorySSACallSite::isRefMod(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  return isMemSSAArgRefMod(m_actual_params[idx], m_only_singleton);
}

// return true if the shadow.mem.XXX instruction associated with the
// idx-th actual paramter is shadow.mem.arg_new.
bool MemorySSACallSite::isNew(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  return isMemSSAArgNew(m_actual_params[idx], m_only_singleton);
}

// return the non-primed top-level variable of the shadow.mem.XXX
// instruction associated with the idx-th actual parameter.
const Value *MemorySSACallSite::getNonPrimed(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    errs() << "Number of actual parameters=" << m_actual_params.size() << "\n";
    errs() << "Accessing index=" << idx << "\n";
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  return getMemSSAParamNonPrimed(m_actual_params[idx], m_only_singleton);
}

// return the primed top-level variable of the shadow.mem.XXX
// instruction associated with the idx-th actual parameter.
const Value *MemorySSACallSite::getPrimed(unsigned idx) const {
  if (idx >= m_actual_params.size()) {
    errs() << "Number of actual parameters=" << m_actual_params.size() << "\n";
    errs() << "Accessing index=" << idx << "\n";
    report_fatal_error("[IP-DSE] out of range access to m_actual_params");
  }
  assert(isRefMod(idx) || isMod(idx) || isNew(idx));
  return getMemSSAParamPrimed(m_actual_params[idx], m_only_singleton);
}

void MemorySSACallSite::write(raw_ostream &o) const {
  // TODO: pretty-printing
  o << *m_ci << "\n";
  for (unsigned i = 0, e = m_actual_params.size(); i < e; ++i) {
    if (m_actual_params[i]) {
      o << "\t" << *(m_actual_params[i]) << "\n";
    }
  }
}

void MemorySSACallSite::dump() const { write(llvm::errs()); }

MemorySSAFunction::MemorySSAFunction(Function &F, Pass &P, bool only_singleton)
    : m_F(F), m_only_singleton(only_singleton) {
  // XXX: We don't need main since it is the root of the call
  // graph so no need to store information about it

  // Find the return block (UnifyFunctionExitNodes ensures at most one)
  BasicBlock *exitBB = nullptr;
  for (auto &B : m_F) {
    if (isa<ReturnInst>(B.getTerminator())) {
      exitBB = &B;
      break;
    }
  }

  if (exitBB) {
    // From the beginning of the block until the return we
    // should have have a bunch of shadow.mem.in and shadow.mem.out
    // calls.
    for (auto const &inst : *exitBB) {
      if (const CallBase *CB = dyn_cast<const CallBase>(&inst)) {
        if (CB->getCalledFunction() && isMemSSAFunIn(CB, m_only_singleton)) {
          int64_t idx = getMemSSAParamIdx(CB);
          if (idx < 0) {
            report_fatal_error(
                "[IP-DSE] Cannot find index in shadow.mem function");
          }
          const Value *in_formal = CB->getArgOperand(1);
          // TODO: if everything is ok the definition of
          // in_formal must be the return value of a call to
          // shadow.mem.arg.init
          m_in_formal_params.insert(std::make_pair((unsigned)idx, in_formal));
        }
      }
    }
  }
}

// Return value can be null if not found
const Value *MemorySSAFunction::getInFormal(unsigned idx) const {
  auto it = m_in_formal_params.find(idx);
  if (it != m_in_formal_params.end())
    return it->second;
  else {
    return nullptr;
  }
}

MemorySSACallsManager::MemorySSACallsManager(Module &M, Pass &P,
                                             bool only_singleton)
    : m_M(M), m_only_singleton(only_singleton) {

  for (auto &F : m_M) {
    if (F.isDeclaration())
      continue;

    m_functions.insert(
        std::make_pair(&F, new MemorySSAFunction(F, P, m_only_singleton)));
    for (auto &I : instructions(&F)) {
      if (CallInst *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            !CI->getCalledFunction()->getName().startswith("shadow.mem")) {
          m_callsites.insert(
              std::make_pair(CI, new MemorySSACallSite(CI, m_only_singleton)));
        }
      }
    }
  }
}

MemorySSACallsManager::~MemorySSACallsManager() {
  for (auto &kv : m_callsites) {
    if (kv.second) {
      delete kv.second;
    }
  }
  for (auto &kv : m_functions) {
    if (kv.second) {
      delete kv.second;
    }
  }
}

const MemorySSAFunction *
MemorySSACallsManager::getFunction(const Function *F) const {
  auto it = m_functions.find(F);
  if (it != m_functions.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}

const MemorySSACallSite *
MemorySSACallsManager::getCallSite(const CallInst *CI) const {
  auto it = m_callsites.find(CI);
  if (it != m_callsites.end()) {
    return it->second;
  } else {
    return nullptr;
  }
}
} // namespace analysis
} // namespace previrt