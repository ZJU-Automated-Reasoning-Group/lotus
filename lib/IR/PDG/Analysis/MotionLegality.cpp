#include "IR/PDG/Analysis/MotionLegality.h"

#include "llvm/IR/Instruction.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace pdg {

namespace {

bool isEdgeAllowed(EdgeType et, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(et);
}

} // namespace

std::set<EdgeType> MotionLegalityQuery::defaultMotionEdgeTypes() {
  return {EdgeType::DATA_DEF_USE,
          EdgeType::DATA_RAW,
          EdgeType::DATA_READ,
          EdgeType::DATA_ALIAS,
          EdgeType::DATA_RET,
          EdgeType::PARAMETER_IN,
          EdgeType::PARAMETER_OUT,
          EdgeType::PARAMETER_FIELD,
          EdgeType::VAL_DEP,
          EdgeType::GLOBAL_DEP,
          EdgeType::CONTROLDEP_CALLINV,
          EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY,
          EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

std::set<EdgeType> MotionLegalityQuery::controlEdgeTypes() {
  return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

MotionLegalityResult
MotionLegalityQuery::canMoveEarlier(Node &moving_node, Node &anchor_node,
                                    const MotionLegalityPolicy &policy) {
  return runCheck(moving_node, anchor_node, MotionDirection::Earlier, policy);
}

MotionLegalityResult
MotionLegalityQuery::canMoveLater(Node &moving_node, Node &anchor_node,
                                  const MotionLegalityPolicy &policy) {
  return runCheck(moving_node, anchor_node, MotionDirection::Later, policy);
}

MotionLegalityResult
MotionLegalityQuery::runCheck(Node &moving_node, Node &anchor_node,
                              MotionDirection direction,
                              const MotionLegalityPolicy &policy) {
  MotionLegalityResult result;
  result.direction = direction;
  result.moving_node = &moving_node;
  result.anchor_node = &anchor_node;

  if (&moving_node == &anchor_node) {
    result.legal = true;
    result.reason = "Trivial move: moving node equals anchor";
    return result;
  }

  if (policy.require_same_function &&
      moving_node.getFunc() != anchor_node.getFunc()) {
    result.reason = "Node motion across functions is disallowed";
    return result;
  }

  std::string movability_reason;
  if (!isMovableInstruction(moving_node, policy, movability_reason)) {
    result.reason = std::move(movability_reason);
    return result;
  }

  const std::set<EdgeType> edge_types =
      policy.edge_types.empty() ? defaultMotionEdgeTypes() : policy.edge_types;

  Node *source = nullptr;
  Node *target = nullptr;
  if (direction == MotionDirection::Earlier) {
    // Moving earlier across anchor is illegal if anchor reaches moving.
    source = &anchor_node;
    target = &moving_node;
  } else {
    // Moving later across anchor is illegal if moving reaches anchor.
    source = &moving_node;
    target = &anchor_node;
  }

  if (findPath(*source, *target, edge_types, result.blocking_path,
               result.blocking_edge_types)) {
    result.reason = direction == MotionDirection::Earlier
                        ? "Anchor transitively constrains moving node"
                        : "Moving node transitively constrains anchor";
    return result;
  }

  if (policy.respect_control_dependence) {
    const std::set<Node *> moving_ctrl = collectControllers(moving_node);
    const std::set<Node *> anchor_ctrl = collectControllers(anchor_node);

    if (!policy.allow_speculation) {
      if (moving_ctrl != anchor_ctrl) {
        result.reason =
            "Control contexts differ and speculative motion is disabled";
        return result;
      }
    } else if (direction == MotionDirection::Later) {
      // Even under speculation, sinking into a stronger control context can
      // suppress executions. Keep this disallowed conservatively.
      if (!std::includes(moving_ctrl.begin(), moving_ctrl.end(),
                         anchor_ctrl.begin(), anchor_ctrl.end())) {
        result.reason =
            "Sinking into incompatible control context may suppress executions";
        return result;
      }
    }
  }

  result.legal = true;
  result.reason = "No blocking dependence found";
  return result;
}

bool MotionLegalityQuery::isMovableInstruction(
    Node &node, const MotionLegalityPolicy &policy, std::string &reason) const {
  auto *val = node.getValue();
  auto *inst = dyn_cast_or_null<Instruction>(val);
  if (inst == nullptr)
    return true;

  if (isa<PHINode>(inst)) {
    reason = "PHI nodes are not movable";
    return false;
  }

  if (inst->isTerminator()) {
    reason = "Terminator instructions are not movable";
    return false;
  }

  if (isa<AllocaInst>(inst)) {
    reason = "Alloca instructions are not movable";
    return false;
  }

  if (!policy.allow_side_effecting_instructions) {
    if (inst->mayHaveSideEffects() || inst->mayReadOrWriteMemory() ||
        inst->mayThrow()) {
      reason = "Potential side effects/memory effects make motion unsafe";
      return false;
    }
  }

  return true;
}

bool MotionLegalityQuery::findPath(
    Node &source, Node &target, const std::set<EdgeType> &edge_types,
    std::vector<Node *> &path, std::vector<EdgeType> &path_edge_types) const {
  path.clear();
  path_edge_types.clear();

  if (&source == &target) {
    path.push_back(&source);
    return true;
  }

  std::queue<Node *> worklist;
  std::unordered_set<Node *> visited;
  std::unordered_map<Node *, Node *> pred;
  std::unordered_map<Node *, EdgeType> pred_edge;

  worklist.push(&source);
  visited.insert(&source);
  pred[&source] = nullptr;

  bool found = false;
  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();

    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *next = edge->getDstNode();
      if (next == nullptr || visited.count(next))
        continue;

      visited.insert(next);
      pred[next] = current;
      pred_edge[next] = edge->getEdgeType();
      if (next == &target) {
        found = true;
        break;
      }
      worklist.push(next);
    }
    if (found)
      break;
  }

  if (!found)
    return false;

  for (Node *n = &target; n != nullptr; n = pred[n]) {
    path.push_back(n);
    if (pred[n] != nullptr)
      path_edge_types.push_back(pred_edge[n]);
  }
  std::reverse(path.begin(), path.end());
  std::reverse(path_edge_types.begin(), path_edge_types.end());
  return true;
}

std::set<Node *> MotionLegalityQuery::collectControllers(Node &node) const {
  const auto ctrl_edges = controlEdgeTypes();
  std::set<Node *> result;
  std::unordered_set<Node *> visited;
  std::queue<Node *> worklist;

  worklist.push(&node);
  visited.insert(&node);

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    for (auto *edge : current->getInEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!ctrl_edges.count(edge->getEdgeType()))
        continue;
      Node *pred = edge->getSrcNode();
      if (pred == nullptr || visited.count(pred))
        continue;
      visited.insert(pred);
      result.insert(pred);
      worklist.push(pred);
    }
  }
  return result;
}

} // namespace pdg
