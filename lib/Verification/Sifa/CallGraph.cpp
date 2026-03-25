//===-- Verification/Sifa/CallGraph.cpp
//------------------------------------===//
//
// Call graph for interprocedural Sifa (Ultimate Library-Sifa port).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/CallGraph.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using namespace lotus::sifa;

namespace {

const llvm::StringRef kDefaultErrorNames[] = {
    "__VERIFIER_error", "__VERIFIER_abort", "abort",          "__assert_fail",
    "llvm.trap",        "llvm.debugtrap",   "__builtin_trap",
};

bool isErrorName(llvm::StringRef calleeName,
                 llvm::ArrayRef<llvm::StringRef> names) {
  if (names.empty()) {
    for (llvm::StringRef n : kDefaultErrorNames)
      if (n == calleeName)
        return true;
    return false;
  }
  for (llvm::StringRef n : names)
    if (n == calleeName)
      return true;
  return false;
}

bool isNoReturnCall(const llvm::CallBase &call) {
  if (call.hasFnAttr(llvm::Attribute::NoReturn)) {
    return true;
  }
  const llvm::Function *callee = call.getCalledFunction();
  return callee && callee->hasFnAttribute(llvm::Attribute::NoReturn);
}

bool isErrorLocation(const llvm::BasicBlock &BB,
                     llvm::ArrayRef<llvm::StringRef> errorNames) {
  bool endsInUnreachable = llvm::isa<llvm::UnreachableInst>(BB.getTerminator());
  for (const llvm::Instruction &I : BB) {
    const auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!call) {
      continue;
    }

    if (const llvm::Function *callee = call->getCalledFunction()) {
      if (isErrorName(callee->getName(), errorNames)) {
        return true;
      }
    }

    if (endsInUnreachable && isNoReturnCall(*call)) {
      return true;
    }
  }
  return false;
}

std::vector<CallGraph::LOI>
gatherErrorLocationsImpl(const llvm::Module &M,
                         llvm::ArrayRef<llvm::StringRef> errorNames) {
  std::vector<CallGraph::LOI> lois;
  for (const llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      if (isErrorLocation(BB, errorNames)) {
        lois.push_back({&F, &BB});
      }
    }
  }
  return lois;
}

} // namespace

std::vector<CallGraph::LOI> CallGraph::gatherErrorLocations(
    const llvm::Module &M, llvm::ArrayRef<llvm::StringRef> errorFunctionNames) {
  return gatherErrorLocationsImpl(M, errorFunctionNames);
}

namespace {

void buildCalls(
    const llvm::Module &M,
    std::unordered_map<const llvm::Function *,
                       std::unordered_set<const llvm::Function *>> &mCalls,
    std::unordered_map<const llvm::Function *,
                       std::unordered_set<const llvm::Function *>> &mCalledBy) {
  for (const llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F) {
      for (const llvm::Instruction &I : BB) {
        auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!call || !call->getCalledFunction())
          continue;
        llvm::Function *callee = call->getCalledFunction();
        if (callee->isDeclaration())
          continue;
        mCalls[&F].insert(callee);
        mCalledBy[callee].insert(&F);
      }
    }
  }
}

bool hasCycle(
    const std::unordered_map<const llvm::Function *,
                             std::unordered_set<const llvm::Function *>>
        &mCalls,
    const std::unordered_set<const llvm::Function *> &closure) {
  std::unordered_set<const llvm::Function *> visited, stack;
  std::function<bool(const llvm::Function *)> dfs =
      [&](const llvm::Function *F) {
        if (stack.count(F))
          return true;
        if (visited.count(F))
          return false;
        visited.insert(F);
        stack.insert(F);
        auto it = mCalls.find(F);
        if (it != mCalls.end()) {
          for (const llvm::Function *callee : it->second) {
            if (closure.count(callee) && dfs(callee))
              return true;
          }
        }
        stack.erase(F);
        return false;
      };
  for (const llvm::Function *F : closure) {
    if (dfs(F))
      return true;
  }
  return false;
}

void topsortCallersFirst(
    const std::unordered_set<const llvm::Function *> &closure,
    const std::unordered_map<const llvm::Function *,
                             std::unordered_set<const llvm::Function *>>
        &mCalls,
    std::vector<const llvm::Function *> &out) {
  std::unordered_set<const llvm::Function *> visited;
  std::function<void(const llvm::Function *)> dfs =
      [&](const llvm::Function *F) {
        if (!closure.count(F) || !visited.insert(F).second)
          return;
        auto it = mCalls.find(F);
        if (it != mCalls.end()) {
          for (const llvm::Function *callee : it->second) {
            if (closure.count(callee))
              dfs(callee);
          }
        }
        out.push_back(F);
      };
  for (const llvm::Function *F : closure) {
    dfs(F);
  }
  std::reverse(out.begin(), out.end());
}

} // namespace

CallGraph::CallGraph(const llvm::Module &M,
                     const llvm::Function *entryProcedure,
                     const std::vector<LOI> &locationsOfInterest)
    : CallGraph(M,
                entryProcedure
                    ? llvm::ArrayRef<const llvm::Function *>{entryProcedure}
                    : llvm::ArrayRef<const llvm::Function *>{},
                locationsOfInterest) {}

CallGraph::CallGraph(const llvm::Module &M,
                     llvm::ArrayRef<const llvm::Function *> initialProcedures,
                     const std::vector<LOI> &locationsOfInterest)
    : M_(&M),
      initialProcedures_(initialProcedures.begin(), initialProcedures.end()) {
  for (const LOI &loi : locationsOfInterest) {
    if (loi.first && loi.second)
      loisInsideProcedure_[loi.first].push_back(loi.second);
  }

  buildCalls(M, mCalls_, mCalledBy_);

  // computeSuccOfInterest: for each procedure with LOI, markPredecessors.
  for (const auto &it : loisInsideProcedure_) {
    const llvm::Function *proc = it.first;
    const std::vector<const llvm::BasicBlock *> &lois = it.second;
    if (lois.empty())
      continue;
    std::function<void(const llvm::Function *)> markPredecessors =
        [&](const llvm::Function *current) {
          auto it = mCalledBy_.find(current);
          if (it == mCalledBy_.end())
            return;
          for (const llvm::Function *caller : it->second) {
            if (successorsOfInterest_[caller].insert(current).second)
              markPredecessors(caller);
          }
        };
    markPredecessors(proc);
  }

  std::vector<const llvm::Function *> initial = initialProceduresOfInterest();
  std::unordered_set<const llvm::Function *> closure = callClosure(initial);
  if (hasCycle(mCalls_, closure)) {
    throw std::invalid_argument("Recursive programs are not supported.");
  }
  topsortCallersFirst(closure, mCalls_, topsorted_);
}

std::vector<const llvm::Function *>
CallGraph::initialProceduresOfInterest() const {
  std::vector<const llvm::Function *> out;
  out.reserve(initialProcedures_.size());
  for (const llvm::Function *entryProcedure : initialProcedures_) {
    if (!entryProcedure || entryProcedure->isDeclaration()) {
      continue;
    }
    if (hasLoiOrSuccessorWithLoi(entryProcedure))
      out.push_back(entryProcedure);
  }
  return out;
}

bool CallGraph::hasLoiOrSuccessorWithLoi(const llvm::Function *F) const {
  auto it = loisInsideProcedure_.find(F);
  if (it != loisInsideProcedure_.end() && !it->second.empty())
    return true;
  auto jt = successorsOfInterest_.find(F);
  return jt != successorsOfInterest_.end() && !jt->second.empty();
}

std::vector<const llvm::BasicBlock *>
CallGraph::locationsOfInterest(const llvm::Function &procedure) const {
  auto it = loisInsideProcedure_.find(&procedure);
  if (it == loisInsideProcedure_.end())
    return {};
  return it->second;
}

std::vector<const llvm::Function *>
CallGraph::successorsOfInterest(const llvm::Function &procedure) const {
  auto it = successorsOfInterest_.find(&procedure);
  if (it == successorsOfInterest_.end())
    return {};
  return std::vector<const llvm::Function *>(it->second.begin(),
                                             it->second.end());
}

const std::vector<const llvm::Function *> &
CallGraph::relevantProceduresTopsorted() const {
  return topsorted_;
}

std::unordered_set<const llvm::Function *> CallGraph::callClosure(
    const std::vector<const llvm::Function *> &procedures) const {
  std::unordered_set<const llvm::Function *> closure;
  std::deque<const llvm::Function *> work(procedures.begin(), procedures.end());
  while (!work.empty()) {
    const llvm::Function *next = work.front();
    work.pop_front();
    if (!closure.insert(next).second)
      continue;
    auto it = mCalls_.find(next);
    if (it != mCalls_.end()) {
      for (const llvm::Function *callee : it->second)
        work.push_back(callee);
    }
  }
  return closure;
}
