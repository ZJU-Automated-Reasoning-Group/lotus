#include "Verification/Sifa/Procedure/ProcedureResources.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include "Utils/Algorithms/PathExpressions/Regex.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Procedure/ProcedureGraphBuilder.h"
#include "Verification/Sifa/RegexDag/RegexDagUtils.h"
#include "Verification/Sifa/Statistics/RegexStatUtils.h"

using namespace lotus::sifa;

ProcedureResources::ProcedureResources(
    SifaStats &stats, const llvm::Function &F,
    const std::vector<llvm::BasicBlock *> &lois) {
  ProcedureGraphBuilder builder(stats, F);
  ProcedureGraph pg =
      builder.graphOfProcedure(lois, /*restrictToReachable=*/true);
  auto *entry = pg.getEntryNode();

  auto pe = createPEComputer(stats, pg.graph());
  auto regexToDag = createRegexToDag<Transition>(stats);

  std::vector<RegexDagNode<Transition> *> loiMarkers;
  loiMarkers.reserve(lois.size());

  std::uint32_t nextMarkerId = 1;
  for (llvm::BasicBlock *loi : lois) {
    auto *loiNode = pg.getBlockEntryNode(*loi);
    auto expr = exprBetween(stats, pe, entry, loiNode);
    auto marked = markRegex(expr, loi, nextMarkerId++);
    loiMarkers.push_back(addToDag(stats, regexToDag, marked));
  }

  auto *const exitNode = pg.getExitNode();
  auto exprToExit = pg.graph().getNodes().count(exitNode)
                        ? exprBetween(stats, pe, entry, exitNode)
                        : lotus::pathexpressions::Regex<Transition>::emptySet();
  auto markedExit =
      markRegex(exprToExit, /*finalLocationAsMark=*/nullptr, nextMarkerId++);
  auto *exitMarker = addToDag(stats, regexToDag, markedExit);

  regexDag_ = getDagAndReset(stats, regexToDag);
  compress(stats, regexDag_);

  for (auto *m : loiMarkers) {
    overlayToLois_.addInclusive(m);
    overlayToLoisAndReturn_.addInclusive(m);
    overlayToLoisAndEnterCalls_.addInclusive(m);
  }

  overlayToReturn_.addInclusive(exitMarker);
  overlayToLoisAndReturn_.addInclusive(exitMarker);
}

ProcedureResources::ProcedureResources(
    SifaStats &stats, const llvm::Function &F,
    const std::vector<llvm::BasicBlock *> &lois,
    const std::vector<const llvm::Function *> &enterCallsOfInterest) {
  ProcedureGraphBuilder builder(stats, F);
  ProcedureGraph pg = builder.graphOfProcedure(lois, enterCallsOfInterest,
                                               /*restrictToReachable=*/true);
  auto *entry = pg.getEntryNode();

  auto pe = createPEComputer(stats, pg.graph());
  auto regexToDag = createRegexToDag<Transition>(stats);

  std::vector<RegexDagNode<Transition> *> loiMarkers;
  loiMarkers.reserve(lois.size());

  // Marker ids must be stable and unique within the DAG's transition alphabet.
  // We start at 1 to avoid the common "0 means uninitialized" convention.
  std::uint32_t nextMarkerId = 1;
  for (llvm::BasicBlock *loi : lois) {
    auto *loiNode = pg.getBlockEntryNode(*loi);
    auto expr = exprBetween(stats, pe, entry, loiNode);
    auto marked = markRegex(expr, loi, nextMarkerId++);
    loiMarkers.push_back(addToDag(stats, regexToDag, marked));
  }

  // Also add one marked regex to the explicit EXIT node (nullptr). If the
  // graph has no return edges at all, Ultimate uses the empty-set regex here.
  auto *const exitNode = pg.getExitNode();
  auto exprToExit = pg.graph().getNodes().count(exitNode)
                        ? exprBetween(stats, pe, entry, exitNode)
                        : lotus::pathexpressions::Regex<Transition>::emptySet();
  auto markedExit =
      markRegex(exprToExit, /*finalLocationAsMark=*/nullptr, nextMarkerId++);
  auto *exitMarker = addToDag(stats, regexToDag, markedExit);

  std::vector<RegexDagNode<Transition> *> enterCallMarkers;
  if (!enterCallsOfInterest.empty()) {
    for (const llvm::Function *callee : enterCallsOfInterest) {
      if (!callee || callee->isDeclaration() || callee->empty()) {
        continue;
      }
      auto *calleeEntry =
          const_cast<llvm::BasicBlock *>(&callee->getEntryBlock());
      auto *calleeEntryNode = pg.getBlockEntryNode(*calleeEntry);
      if (!calleeEntryNode) {
        continue;
      }
      auto expr = exprBetween(stats, pe, entry, calleeEntryNode);
      auto marked = markRegex(expr, calleeEntry, nextMarkerId++);
      enterCallMarkers.push_back(addToDag(stats, regexToDag, marked));
    }
  }

  regexDag_ = getDagAndReset(stats, regexToDag);
  compress(stats, regexDag_);

  for (auto *m : loiMarkers) {
    overlayToLois_.addInclusive(m);
  }

  overlayToReturn_.addInclusive(exitMarker);

  for (auto *m : loiMarkers) {
    overlayToLoisAndReturn_.addInclusive(m);
  }
  overlayToLoisAndReturn_.addInclusive(exitMarker);
  for (auto *m : enterCallMarkers) {
    overlayToLoisAndReturn_.addExclusive(m);
  }

  // overlayToLoisAndEnterCalls_: LOI markers (inclusive) + enter-call markers
  // (exclusive). Does NOT include the EXIT marker — the interpreter stops at
  // call sites rather than propagating through to return.
  for (auto *m : loiMarkers) {
    overlayToLoisAndEnterCalls_.addInclusive(m);
  }
  for (auto *m : enterCallMarkers) {
    overlayToLoisAndEnterCalls_.addExclusive(m);
  }
}

const RegexDag<Transition> &ProcedureResources::getRegexDag() const {
  return regexDag_;
}
const BackwardClosedOverlay<Transition> &
ProcedureResources::getDagOverlayPathToLois() const {
  return overlayToLois_;
}
const BackwardClosedOverlay<Transition> &
ProcedureResources::getDagOverlayPathToReturn() const {
  return overlayToReturn_;
}
const BackwardClosedOverlay<Transition> &
ProcedureResources::getDagOverlayPathToLoisAndReturn() const {
  return overlayToLoisAndReturn_;
}
const BackwardClosedOverlay<Transition> &
ProcedureResources::getDagOverlayPathToLoisAndEnterCalls() const {
  return overlayToLoisAndEnterCalls_;
}
