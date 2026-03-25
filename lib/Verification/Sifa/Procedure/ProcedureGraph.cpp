#include "Verification/Sifa/Procedure/ProcedureGraph.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <cstddef>
#include <utility>

using namespace lotus::sifa;

namespace {

const llvm::Instruction *firstNonPhi(const llvm::BasicBlock &BB) {
  for (const llvm::Instruction &I : BB) {
    if (!llvm::isa<llvm::PHINode>(&I)) {
      return &I;
    }
  }
  return nullptr;
}

bool exitsViaReturn(const llvm::BasicBlock &BB) {
  return llvm::isa<llvm::ReturnInst>(BB.getTerminator());
}

} // namespace

std::size_t
ProcedureGraph::NodePairHash::operator()(const std::pair<Node, Node> &p) const {
  const std::size_t a = std::hash<Node>()(p.first);
  const std::size_t b = std::hash<Node>()(p.second);
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

ProcedureGraph::Node ProcedureGraph::getEntryNode() const { return entryNode_; }
ProcedureGraph::Node ProcedureGraph::getExitNode() const { return exitNode_; }

void ProcedureGraph::setEntryNode(Node n) {
  entryNode_ = n;
  if (entryNode_) {
    graph_.addNode(entryNode_);
  }
}

ProcedureGraph::Node
ProcedureGraph::getBlockEntryNode(const llvm::BasicBlock &bb) const {
  auto it = blockEntryNodes_.find(&bb);
  return it == blockEntryNodes_.end() ? nullptr : it->second;
}

ProcedureGraph::Node ProcedureGraph::createNode(llvm::BasicBlock *bb,
                                                std::uint32_t ordinal) {
  ownedNodes_.emplace_back(new ProgramPoint{bb, ordinal});
  return ownedNodes_.back().get();
}

ProcedureGraph::Node
ProcedureGraph::getOrCreateBlockEntryNode(llvm::BasicBlock *bb) {
  if (!bb) {
    return nullptr;
  }
  auto it = blockEntryNodes_.find(bb);
  if (it != blockEntryNodes_.end()) {
    return it->second;
  }
  Node node = createNode(bb, 0);
  blockEntryNodes_.emplace(bb, node);
  nextOrdinal_[bb] = 1;
  graph_.addNode(node);
  return node;
}

ProcedureGraph::Node ProcedureGraph::createInternalNode(llvm::BasicBlock *bb) {
  if (!bb) {
    return nullptr;
  }
  std::uint32_t ordinal = nextOrdinal_[bb]++;
  Node node = createNode(bb, ordinal);
  graph_.addNode(node);
  return node;
}

const ProcedureGraph::Graph &ProcedureGraph::graph() const { return graph_; }
const std::vector<TransitionInfo> &ProcedureGraph::transitions() const {
  return transitions_;
}

Transition ProcedureGraph::addTransition(Node src, Node dst,
                                         const llvm::Instruction *segmentStart,
                                         const llvm::Instruction *stopBefore) {
  const std::pair<Node, Node> k{src, dst};
  const auto it = edgeToId_.find(k);
  if (it != edgeToId_.end()) {
    return Transition::makeEdge(it->second, src ? src->block : nullptr,
                                dst ? dst->block : nullptr,
                                src ? src->ordinal : 0, dst ? dst->ordinal : 0,
                                segmentStart, stopBefore);
  }

  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(
      TransitionInfo{src ? src->block : nullptr, dst ? dst->block : nullptr,
                     src ? src->ordinal : 0, dst ? dst->ordinal : 0, nullptr,
                     segmentStart, stopBefore, nullptr});
  edgeToId_.emplace(std::move(k), id);
  return Transition::makeEdge(
      id, src ? src->block : nullptr, dst ? dst->block : nullptr,
      src ? src->ordinal : 0, dst ? dst->ordinal : 0, segmentStart, stopBefore);
}

ProcedureGraph::ProcedureGraph(const llvm::Function &F) {
  exitNode_ = nullptr;
  entryNode_ = getOrCreateBlockEntryNode(
      const_cast<llvm::BasicBlock *>(&F.getEntryBlock()));

  for (const llvm::BasicBlock &BB : F) {
    auto *srcBB = const_cast<llvm::BasicBlock *>(&BB);
    Node src = getOrCreateBlockEntryNode(srcBB);
    const llvm::Instruction *segmentStart = firstNonPhi(BB);

    if (exitsViaReturn(BB)) {
      addEdge(src, exitNode_, segmentStart, nullptr);
      continue;
    }

    for (const llvm::BasicBlock *succBB : llvm::successors(&BB)) {
      Node dst =
          getOrCreateBlockEntryNode(const_cast<llvm::BasicBlock *>(succBB));
      addEdge(src, dst, segmentStart, nullptr);
    }
  }
}

void ProcedureGraph::addNode(Node n) {
  if (n) {
    graph_.addNode(n);
  }
}

void ProcedureGraph::addEdge(Node src, Node dst,
                             const llvm::Instruction *segmentStart,
                             const llvm::Instruction *stopBefore) {
  if (!src) {
    return;
  }
  const auto label = addTransition(src, dst, segmentStart, stopBefore);
  graph_.addEdge(src, label, dst);
}

void ProcedureGraph::addReturnSummaryEdge(Node src, Node dst,
                                          const llvm::Function *callee,
                                          const llvm::CallBase *callSite) {
  if (!src || !dst || !callee) {
    return;
  }
  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(TransitionInfo{
      src->block, dst->block, src->ordinal, dst->ordinal,
      const_cast<llvm::Function *>(callee), nullptr, nullptr, callSite});
  const auto label = Transition::makeReturnSummary(
      id, src->block, dst->block, src->ordinal, dst->ordinal, callee, callSite);
  graph_.addEdge(src, label, dst);
}

void ProcedureGraph::addEnterCallEdge(Node src, Node dst,
                                      const llvm::Function *callee,
                                      const llvm::CallBase *callSite) {
  if (!src || !dst || !callee) {
    return;
  }
  const std::uint32_t id = static_cast<std::uint32_t>(transitions_.size());
  transitions_.push_back(TransitionInfo{
      src->block, dst->block, src->ordinal, dst->ordinal,
      const_cast<llvm::Function *>(callee), nullptr, nullptr, callSite});
  const auto label = Transition::makeEnterCall(
      id, src->block, dst->block, src->ordinal, dst->ordinal, callee, callSite);
  graph_.addEdge(src, label, dst);
}
