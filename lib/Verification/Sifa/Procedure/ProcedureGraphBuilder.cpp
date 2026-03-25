#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

#include <queue>
#include <unordered_map>
#include <unordered_set>

using namespace lotus::sifa;

namespace {

using Block = llvm::BasicBlock *;

const llvm::Instruction *firstNonPhi(Block bb) {
  if (!bb) {
    return nullptr;
  }
  for (const llvm::Instruction &I : *bb) {
    if (!llvm::isa<llvm::PHINode>(&I)) {
      return &I;
    }
  }
  return nullptr;
}

const llvm::Instruction *nextInstruction(const llvm::Instruction &I) {
  auto it = I.getIterator();
  ++it;
  if (it == I.getParent()->end()) {
    return nullptr;
  }
  return &*it;
}

bool exitsViaReturn(Block bb) {
  return bb && llvm::isa<llvm::ReturnInst>(bb->getTerminator());
}

llvm::Function *implementedDirectCallee(const llvm::Instruction &I) {
  auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
  if (!call) {
    return nullptr;
  }
  llvm::Function *callee = call->getCalledFunction();
  if (!callee || callee->isDeclaration()) {
    return nullptr;
  }
  return callee;
}

void addTailEdges(ProcedureGraph &pg, ProcedureGraph::Node srcNode, Block srcBB,
                  const llvm::Instruction *segmentStart,
                  const std::unordered_set<Block> &reachable) {
  if (!srcNode || !srcBB) {
    return;
  }

  if (exitsViaReturn(srcBB)) {
    if (reachable.count(nullptr)) {
      pg.addEdge(srcNode, pg.getExitNode(), segmentStart, nullptr);
    }
    return;
  }

  for (const llvm::BasicBlock *succBB : llvm::successors(srcBB)) {
    Block succ = const_cast<llvm::BasicBlock *>(succBB);
    if (!reachable.count(succ)) {
      continue;
    }
    pg.addEdge(srcNode, pg.getBlockEntryNode(*succ), segmentStart, nullptr);
  }
}

} // namespace

ProcedureGraphBuilder::ProcedureGraphBuilder(SifaStats &stats,
                                             const llvm::Function &F)
    : stats_(stats), F_(F) {}

ProcedureGraph ProcedureGraphBuilder::graphOfProcedure(
    const std::vector<llvm::BasicBlock *> &locationsOfInterest,
    bool restrictToReachable) {
  stats_.start(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME);
  struct StopTimer {
    SifaStats &stats;
    ~StopTimer() { stats.stop(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME); }
  } stopTimer{stats_};

  if (!restrictToReachable || locationsOfInterest.empty()) {
    return ProcedureGraph(F_);
  }

  std::unordered_set<Block> reachable;
  std::queue<Block> work;
  work.push(nullptr);
  reachable.insert(nullptr);
  for (Block loi : locationsOfInterest) {
    if (loi && reachable.insert(loi).second) {
      work.push(loi);
    }
  }

  while (!work.empty()) {
    Block cur = work.front();
    work.pop();
    if (!cur) {
      for (const llvm::BasicBlock &BB : F_) {
        if (exitsViaReturn(const_cast<llvm::BasicBlock *>(&BB))) {
          Block pred = const_cast<llvm::BasicBlock *>(&BB);
          if (reachable.insert(pred).second) {
            work.push(pred);
          }
        }
      }
      continue;
    }

    for (Block pred : llvm::predecessors(cur)) {
      if (reachable.insert(pred).second) {
        work.push(pred);
      }
    }
  }

  ProcedureGraph pg;
  pg.setEntryNode(pg.getOrCreateBlockEntryNode(
      const_cast<llvm::BasicBlock *>(&F_.getEntryBlock())));
  for (Block bb : reachable) {
    if (bb) {
      pg.getOrCreateBlockEntryNode(bb);
    }
  }
  for (const llvm::BasicBlock &BB : F_) {
    Block srcBB = const_cast<llvm::BasicBlock *>(&BB);
    if (!reachable.count(srcBB)) {
      continue;
    }
    addTailEdges(pg, pg.getBlockEntryNode(BB), srcBB, firstNonPhi(srcBB),
                 reachable);
  }
  return pg;
}

ProcedureGraph ProcedureGraphBuilder::graphOfProcedure(
    const std::vector<llvm::BasicBlock *> &locationsOfInterest,
    const std::vector<const llvm::Function *> &enterCallsOfInterest,
    bool restrictToReachable) {
  stats_.start(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME);
  struct StopTimer {
    SifaStats &stats;
    ~StopTimer() { stats.stop(SifaStats::Key::PROCEDURE_GRAPH_BUILDER_TIME); }
  } stopTimer{stats_};

  Block entry = const_cast<llvm::BasicBlock *>(&F_.getEntryBlock());

  std::unordered_set<const llvm::Function *> enterCallSet(
      enterCallsOfInterest.begin(), enterCallsOfInterest.end());
  std::unordered_map<const llvm::Function *, std::vector<Block>> enterCallPreds;
  std::unordered_map<Block, std::vector<Block>> summaryPreds;
  std::unordered_set<Block> requestedEnterTargets;

  for (const llvm::BasicBlock &BB : F_) {
    Block src = const_cast<llvm::BasicBlock *>(&BB);
    for (const llvm::Instruction &I : BB) {
      llvm::Function *callee = implementedDirectCallee(I);
      if (!callee) {
        continue;
      }

      if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
        summaryPreds[const_cast<llvm::BasicBlock *>(invoke->getNormalDest())]
            .push_back(src);
      } else if (exitsViaReturn(src)) {
        summaryPreds[nullptr].push_back(src);
      } else {
        for (const llvm::BasicBlock *succBB : llvm::successors(&BB)) {
          summaryPreds[const_cast<llvm::BasicBlock *>(succBB)].push_back(src);
        }
      }

      if (enterCallSet.count(callee) && !callee->empty()) {
        Block calleeEntry =
            const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
        enterCallPreds[callee].push_back(src);
        requestedEnterTargets.insert(calleeEntry);
      }
    }
  }

  std::unordered_set<Block> reachable;
  if (!restrictToReachable) {
    for (const llvm::BasicBlock &BB : F_) {
      reachable.insert(const_cast<llvm::BasicBlock *>(&BB));
    }
    for (Block enterTarget : requestedEnterTargets) {
      reachable.insert(enterTarget);
    }
    for (const llvm::BasicBlock &BB : F_) {
      if (exitsViaReturn(const_cast<llvm::BasicBlock *>(&BB))) {
        reachable.insert(nullptr);
        break;
      }
    }
  } else {
    std::queue<Block> work;
    work.push(nullptr);
    reachable.insert(nullptr);
    for (Block loi : locationsOfInterest) {
      if (loi && reachable.insert(loi).second) {
        work.push(loi);
      }
    }
    for (Block enterTarget : requestedEnterTargets) {
      if (reachable.insert(enterTarget).second) {
        work.push(enterTarget);
      }
    }

    while (!work.empty()) {
      Block cur = work.front();
      work.pop();
      if (!cur) {
        for (const llvm::BasicBlock &BB : F_) {
          if (exitsViaReturn(const_cast<llvm::BasicBlock *>(&BB))) {
            Block pred = const_cast<llvm::BasicBlock *>(&BB);
            if (reachable.insert(pred).second) {
              work.push(pred);
            }
          }
        }
        continue;
      }

      if (cur->getParent() != &F_) {
        const llvm::Function *callee = cur->getParent();
        auto it = enterCallPreds.find(callee);
        if (it == enterCallPreds.end()) {
          continue;
        }
        for (Block pred : it->second) {
          if (reachable.insert(pred).second) {
            work.push(pred);
          }
        }
        continue;
      }

      for (Block pred : llvm::predecessors(cur)) {
        if (reachable.insert(pred).second) {
          work.push(pred);
        }
      }
      auto sumIt = summaryPreds.find(cur);
      if (sumIt != summaryPreds.end()) {
        for (Block pred : sumIt->second) {
          if (reachable.insert(pred).second) {
            work.push(pred);
          }
        }
      }
    }
  }

  ProcedureGraph pg;
  pg.setEntryNode(pg.getOrCreateBlockEntryNode(entry));
  for (Block bb : reachable) {
    if (bb) {
      pg.getOrCreateBlockEntryNode(bb);
    }
  }

  for (const llvm::BasicBlock &BB : F_) {
    Block srcBB = const_cast<llvm::BasicBlock *>(&BB);
    if (!reachable.count(srcBB)) {
      continue;
    }

    ProcedureGraph::Node cur = pg.getBlockEntryNode(BB);
    const llvm::Instruction *segmentStart = firstNonPhi(srcBB);
    for (const llvm::Instruction &I : BB) {
      llvm::Function *callee = implementedDirectCallee(I);
      if (!callee) {
        continue;
      }
      auto *call = llvm::cast<llvm::CallBase>(&I);

      if (segmentStart && segmentStart != &I) {
        ProcedureGraph::Node beforeCall = pg.createInternalNode(srcBB);
        pg.addEdge(cur, beforeCall, segmentStart, &I);
        cur = beforeCall;
      }

      if (const auto *invoke = llvm::dyn_cast<llvm::InvokeInst>(call)) {
        Block normalDest =
            const_cast<llvm::BasicBlock *>(invoke->getNormalDest());
        if (reachable.count(normalDest)) {
          pg.addReturnSummaryEdge(cur, pg.getBlockEntryNode(*normalDest),
                                  callee, call);
        }
        Block unwindDest =
            const_cast<llvm::BasicBlock *>(invoke->getUnwindDest());
        if (reachable.count(unwindDest)) {
          pg.addEdge(cur, pg.getBlockEntryNode(*unwindDest), &I, nullptr);
        }
        if (enterCallSet.count(callee) && !callee->empty()) {
          Block calleeEntry =
              const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
          if (reachable.count(calleeEntry)) {
            pg.addEnterCallEdge(cur, pg.getBlockEntryNode(*calleeEntry), callee,
                                call);
          }
        }
        cur = nullptr;
        segmentStart = nullptr;
        break;
      }

      ProcedureGraph::Node afterCall = pg.createInternalNode(srcBB);
      pg.addReturnSummaryEdge(cur, afterCall, callee, call);
      if (enterCallSet.count(callee) && !callee->empty()) {
        Block calleeEntry =
            const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
        if (reachable.count(calleeEntry)) {
          pg.addEnterCallEdge(cur, pg.getBlockEntryNode(*calleeEntry), callee,
                              call);
        }
      }

      cur = afterCall;
      segmentStart = nextInstruction(I);
    }

    if (!cur) {
      continue;
    }
    addTailEdges(pg, cur, srcBB, segmentStart, reachable);
  }

  return pg;
}
