#include "Concurrency/Thread/ThreadCreationTree.h"

#include "Concurrency/Utils/ThreadAPI.h"

#include <algorithm>
#include <functional>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

using namespace llvm;

namespace lotus::analysis {

ThreadCallGraph::ThreadCallGraph(Module &module, ThreadAPI &threadAPI,
                                 const InstructionScope *scope)
    : module_(&module), threadAPI_(&threadAPI), scope_(scope) {
  build();
}

bool ThreadCallGraph::retained(const Instruction *instruction) const {
  return instruction && (!scope_ || scope_->count(instruction) != 0);
}

void ThreadCallGraph::build() {
  auto indirectTargets = [&](const Value *calledValue) {
    std::vector<const Function *> targets;
    if (!calledValue)
      return targets;
    const FunctionType *expected = nullptr;
    if (calledValue && calledValue->getType()->isPointerTy())
      expected = dyn_cast<FunctionType>(
          calledValue->getType()->getPointerElementType());
    for (const Function &candidate : *module_) {
      if (candidate.isDeclaration() || candidate.isIntrinsic())
        continue;
      if (expected && candidate.getFunctionType() != expected)
        continue;
      targets.push_back(&candidate);
    }
    return targets;
  };

  for (Function &function : *module_) {
    if (function.isDeclaration())
      continue;
    for (Instruction &instruction : instructions(function)) {
      auto *call = dyn_cast<CallBase>(&instruction);
      if (!call || !retained(call))
        continue;
      if (threadAPI_->isTDJoin(call)) {
        joinSites_.push_back(call);
        continue;
      }
      if (threadAPI_->isTDFork(call)) {
        const Value *targetValue = threadAPI_->getForkedFun(call);
        const auto *target = dyn_cast_or_null<Function>(
            targetValue ? targetValue->stripPointerCasts() : nullptr);
        if (target && !target->isDeclaration()) {
          Edge edge{&function, target, call, EdgeKind::Fork};
          edges_.push_back(edge);
          outgoing_[&function].push_back(edge);
        } else {
          for (const Function *candidate : indirectTargets(targetValue)) {
            Edge edge{&function, candidate, call, EdgeKind::Fork};
            edges_.push_back(edge);
            outgoing_[&function].push_back(edge);
          }
        }
        continue;
      }
      const Function *target = call->getCalledFunction();
      if (!target)
        target =
            dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
      if (!target) {
        for (const Function *candidate :
             indirectTargets(call->getCalledOperand())) {
          Edge edge{&function, candidate, call, EdgeKind::Call};
          edges_.push_back(edge);
          outgoing_[&function].push_back(edge);
        }
        continue;
      }
      if (target->isDeclaration() || target->isIntrinsic())
        continue;
      Edge edge{&function, target, call, EdgeKind::Call};
      edges_.push_back(edge);
      outgoing_[&function].push_back(edge);
    }
  }
}

const std::vector<ThreadCallGraph::Edge> &
ThreadCallGraph::outgoing(const Function *function) const {
  static const std::vector<Edge> empty;
  auto it = outgoing_.find(function);
  return it == outgoing_.end() ? empty : it->second;
}

ThreadCreationTree::ThreadCreationTree(Module &module, ThreadAPI &threadAPI,
                                       std::size_t contextLimit,
                                       const InstructionScope *scope)
    : module_(&module), threadAPI_(&threadAPI), contextLimit_(contextLimit),
      scope_(scope), callGraph_(module, threadAPI, scope) {
  build();
}

const Value *
ThreadCreationTree::canonicalThreadHandle(const Value *value) const {
  if (!value)
    return nullptr;
  value = value->stripPointerCasts();
  for (unsigned depth = 0; depth < 8; ++depth) {
    const auto *argument = dyn_cast<Argument>(value);
    if (!argument)
      break;
    const Function *owner = argument->getParent();
    const Value *uniqueActual = nullptr;
    bool ambiguous = false;
    for (const Function &function : *module_) {
      for (const Instruction &instruction : instructions(function)) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        if (!call || call->getCalledFunction() != owner ||
            argument->getArgNo() >= call->arg_size())
          continue;
        const Value *actual =
            call->getArgOperand(argument->getArgNo())->stripPointerCasts();
        if (!uniqueActual)
          uniqueActual = actual;
        else if (uniqueActual != actual)
          ambiguous = true;
      }
    }
    if (!uniqueActual || ambiguous)
      break;
    value = uniqueActual;
  }
  if (value->getType()->isPointerTy())
    if (const Value *base = getUnderlyingObject(value, 32))
      value = base->stripPointerCasts();
  return value;
}

bool ThreadCreationTree::forkMayRepeat(const CallBase *forkSite) const {
  if (!forkSite || !forkSite->getFunction())
    return true;
  DominatorTree dominators(*const_cast<Function *>(forkSite->getFunction()));
  LoopInfo loops(dominators);
  return loops.getLoopFor(forkSite->getParent()) != nullptr;
}

void ThreadCreationTree::build() {
  const Function *entry = module_->getFunction("main");
  if (!entry || entry->isDeclaration()) {
    for (const Function &function : *module_) {
      if (!function.isDeclaration()) {
        entry = &function;
        break;
      }
    }
  }
  if (!entry)
    return;

  Node root;
  root.id = 0;
  root.entry = entry;
  nodes_.push_back(std::move(root));
  expandThread(0, {});
  resolveJoins();

  stats_.nodes = nodes_.size();
  stats_.forkRelations = forkRelations_.size();
  stats_.joinSites = callGraph_.joinSites().size();
  for (const Node &node : nodes_) {
    stats_.multiInstanceNodes += node.multiInstance ? 1U : 0U;
    stats_.cyclicNodes += node.inCycle ? 1U : 0U;
  }
}

void ThreadCreationTree::expandThread(
    ThreadID id, std::unordered_set<const Function *> ancestors) {
  if (id >= nodes_.size())
    return;
  const Function *entry = nodes_[id].entry;
  if (!entry)
    return;
  if (!ancestors.insert(entry).second) {
    nodes_[id].inCycle = true;
    nodes_[id].multiInstance = true;
    return;
  }

  std::unordered_set<const Function *> activeCalls;
  walkCalls(id, entry, nodes_[id].context, activeCalls);

  std::vector<ThreadID> children;
  for (const ForkRelation &relation : forkRelations_)
    if (relation.parent == id)
      children.push_back(relation.child);
  for (ThreadID child : children) {
    if (child >= nodes_.size())
      continue;
    if (ancestors.count(nodes_[child].entry) != 0) {
      nodes_[child].inCycle = true;
      nodes_[child].multiInstance = true;
      continue;
    }
    expandThread(child, ancestors);
  }
}

void ThreadCreationTree::walkCalls(
    ThreadID id, const Function *function,
    std::vector<const CallBase *> context,
    std::unordered_set<const Function *> &activeCalls) {
  if (!function || id >= nodes_.size())
    return;
  nodes_[id].reachableFunctions.insert(function);
  functionInstances_[function].push_back(id);
  const bool recursive = !activeCalls.insert(function).second;
  if (recursive) {
    nodes_[id].multiInstance = true;
    return;
  }

  for (const ThreadCallGraph::Edge &edge : callGraph_.outgoing(function)) {
    std::vector<const CallBase *> nextContext = context;
    nextContext.push_back(edge.callSite);
    if (nextContext.size() > contextLimit_)
      nextContext.erase(nextContext.begin(), nextContext.end() - contextLimit_);
    if (edge.kind == ThreadCallGraph::EdgeKind::Call) {
      walkCalls(id, edge.target, std::move(nextContext), activeCalls);
      continue;
    }
    addChild(id, edge, nextContext, recursive || forkMayRepeat(edge.callSite));
  }
  activeCalls.erase(function);
}

ThreadCreationTree::ThreadID
ThreadCreationTree::addChild(ThreadID parent, const ThreadCallGraph::Edge &edge,
                             const std::vector<const CallBase *> &context,
                             bool repeatedContext) {
  for (const ForkRelation &relation : forkRelations_) {
    if (relation.parent == parent && relation.site == edge.callSite &&
        relation.target == edge.target) {
      nodes_[relation.child].multiInstance = true;
      return relation.child;
    }
  }

  Node child;
  child.id = nodes_.size();
  child.parent = parent;
  child.entry = edge.target;
  child.forkSite = edge.callSite;
  child.context = context;
  child.multiInstance = repeatedContext;
  nodes_.push_back(std::move(child));
  forkRelations_.push_back(
      {parent, nodes_.back().id, edge.callSite, edge.target});

  const Value *handle =
      canonicalThreadHandle(threadAPI_->getForkedThread(edge.callSite));
  if (handle)
    handleThreads_[handle].push_back(nodes_.back().id);
  return nodes_.back().id;
}

void ThreadCreationTree::resolveJoins() {
  for (const CallBase *join : callGraph_.joinSites()) {
    const Value *handle =
        canonicalThreadHandle(threadAPI_->getJoinedThread(join));
    if (!handle)
      continue;
    auto it = handleThreads_.find(handle);
    if (it == handleThreads_.end())
      continue;
    joinThreads_[join] = it->second;
    stats_.resolvedJoins += it->second.size();
  }
}

std::vector<ThreadCreationTree::ThreadID>
ThreadCreationTree::instancesForFunction(const Function *function) const {
  auto it = functionInstances_.find(function);
  if (it == functionInstances_.end())
    return {};
  std::vector<ThreadID> result = it->second;
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<ThreadCreationTree::ThreadID>
ThreadCreationTree::joinedThreads(const CallBase *joinSite) const {
  auto it = joinThreads_.find(joinSite);
  return it == joinThreads_.end() ? std::vector<ThreadID>{} : it->second;
}

std::vector<const Function *>
ThreadCreationTree::joinedFunctions(const CallBase *joinSite) const {
  std::vector<const Function *> functions;
  for (ThreadID id : joinedThreads(joinSite))
    if (id < nodes_.size() && nodes_[id].entry)
      functions.push_back(nodes_[id].entry);
  return functions;
}

bool ThreadCreationTree::joinedBefore(ThreadID child,
                                      const Instruction *instruction) const {
  if (!instruction || !instruction->getFunction())
    return false;
  DominatorTree dominators(*const_cast<Function *>(instruction->getFunction()));
  for (const auto &[join, threads] : joinThreads_) {
    if (!join || join->getFunction() != instruction->getFunction() ||
        std::find(threads.begin(), threads.end(), child) == threads.end())
      continue;
    if (dominators.dominates(join, instruction))
      return true;
  }
  return false;
}

bool ThreadCreationTree::mayOverlap(const Instruction *lhs,
                                    const Instruction *rhs) const {
  if (!lhs || !rhs)
    return true;
  std::vector<ThreadID> lhsInstances = instancesForFunction(lhs->getFunction());
  std::vector<ThreadID> rhsInstances = instancesForFunction(rhs->getFunction());
  if (lhsInstances.empty() || rhsInstances.empty())
    return true;
  for (ThreadID left : lhsInstances) {
    for (ThreadID right : rhsInstances) {
      if (left == right) {
        if (left < nodes_.size() && nodes_[left].multiInstance)
          return true;
        continue;
      }
      if (!joinedBefore(left, rhs) && !joinedBefore(right, lhs))
        return true;
    }
  }
  return false;
}

ThreadCreationTree::ThreadID
ThreadCreationTree::uniqueThreadFor(const Instruction *instruction) const {
  if (!instruction)
    return InvalidThread;
  std::vector<ThreadID> instances =
      instancesForFunction(instruction->getFunction());
  return instances.size() == 1 ? instances.front() : InvalidThread;
}

bool TCTMHPAnalysis::retained(const Instruction *instruction) const {
  return instruction && (!scope_ || scope_->count(instruction) != 0);
}

bool TCTMHPAnalysis::mayHappenInParallel(const Instruction *lhs,
                                         const Instruction *rhs) const {
  return retained(lhs) && retained(rhs) && tree_->mayOverlap(lhs, rhs) &&
         base_->mayHappenInParallel(lhs, rhs);
}

bool TCTMHPAnalysis::isPrecomputedMHP(const Instruction *lhs,
                                      const Instruction *rhs) const {
  return mayHappenInParallel(lhs, rhs);
}

mhp::InstructionSet
TCTMHPAnalysis::getParallelInstructions(const Instruction *instruction) const {
  mhp::InstructionSet result;
  for (const Instruction *candidate :
       base_->getParallelInstructions(instruction))
    if (mayHappenInParallel(instruction, candidate))
      result.insert(candidate);
  return result;
}

bool TCTMHPAnalysis::mustBeSequential(const Instruction *lhs,
                                      const Instruction *rhs) const {
  return !mayHappenInParallel(lhs, rhs);
}

mhp::ThreadID
TCTMHPAnalysis::getThreadID(const Instruction *instruction) const {
  const ThreadCreationTree::ThreadID id = tree_->uniqueThreadFor(instruction);
  return id == ThreadCreationTree::InvalidThread
             ? base_->getThreadID(instruction)
             : id;
}

mhp::InstructionSet
TCTMHPAnalysis::getInstructionsInThread(mhp::ThreadID id) const {
  mhp::InstructionSet result;
  for (const Instruction *instruction : base_->getInstructionsInThread(id))
    if (retained(instruction))
      result.insert(instruction);
  return result;
}

std::size_t TCTMHPAnalysis::getMhpPairCount() const {
  return base_->getMhpPairCount();
}

void TCTMHPAnalysis::printStatistics(raw_ostream &os) const {
  base_->printStatistics(os);
}

void TCTMHPAnalysis::printResults(raw_ostream &os) const {
  base_->printResults(os);
}

} // namespace lotus::analysis
