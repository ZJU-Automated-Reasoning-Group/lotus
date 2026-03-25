#include "IR/PDG/Analysis/PDGQuery.h"

#include "IR/PDG/Analysis/CypherQuery.h"
#include "IR/PDG/Support/PDGUtils.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

using namespace llvm;

namespace pdg {

namespace {

typedef PDGQueryResult::NodeSet NodeSet;
typedef PDGQueryResult::EdgeSet EdgeSet;

struct TraversalOutcome {
  NodeSet nodes;
  EdgeSet edges;
  std::unordered_map<Node *, std::set<Node *>> predecessors;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
      predecessor_edges;
  std::unordered_map<Node *, size_t> distances;
};

struct TraversalState {
  Node *node = nullptr;
  size_t depth = 0;
  std::vector<Node *> call_stack;
};

struct StateHash {
  size_t operator()(const std::pair<Node *, std::vector<Node *>> &value) const {
    size_t seed = std::hash<Node *>()(value.first);
    for (Node *node : value.second) {
      seed ^= std::hash<Node *>()(node) + 0x9e3779b9 + (seed << 6) +
              (seed >> 2);
    }
    return seed;
  }
};

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

static bool isControlEdge(EdgeType type) {
  return type == EdgeType::CONTROLDEP_CALLINV ||
         type == EdgeType::CONTROLDEP_CALLRET ||
         type == EdgeType::CONTROLDEP_ENTRY ||
         type == EdgeType::CONTROLDEP_BR ||
         type == EdgeType::CONTROLDEP_IND_BR;
}

static bool isParameterEdge(EdgeType type) {
  return type == EdgeType::PARAMETER_IN || type == EdgeType::PARAMETER_OUT ||
         type == EdgeType::PARAMETER_FIELD;
}

static bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(type) != 0;
}

static bool usesValueAsPointerOperand(const Value *value,
                                      const Instruction *user) {
  if (value == nullptr || user == nullptr)
    return false;
  if (const auto *load = dyn_cast<LoadInst>(user))
    return load->getPointerOperand() == value;
  if (const auto *store = dyn_cast<StoreInst>(user))
    return store->getPointerOperand() == value;
  if (const auto *gep = dyn_cast<GetElementPtrInst>(user))
    return gep->getPointerOperand() == value;
  return false;
}

static bool isFieldAccess(Node *node) {
  if (node == nullptr || node->getValue() == nullptr)
    return false;
  return isa<LoadInst>(node->getValue()) || isa<StoreInst>(node->getValue()) ||
         isa<GetElementPtrInst>(node->getValue());
}

static bool isValueFlowEdge(Edge *edge, Node *src_node, Node *dst_node) {
  if (edge == nullptr || src_node == nullptr || dst_node == nullptr)
    return false;

  switch (edge->getEdgeType()) {
  case EdgeType::DATA_RET:
  case EdgeType::VAL_DEP:
  case EdgeType::DATA_ALIAS:
  case EdgeType::PARAMETER_IN:
  case EdgeType::PARAMETER_OUT:
  case EdgeType::PARAMETER_FIELD:
    return true;
  case EdgeType::DATA_DEF_USE:
  case EdgeType::DATA_RAW:
  case EdgeType::DATA_READ:
    break;
  default:
    return false;
  }

  const Value *src_value = src_node->getValue();
  const Value *dst_value = dst_node->getValue();
  const Instruction *dst_inst = dyn_cast_or_null<Instruction>(dst_value);
  const Instruction *src_inst = dyn_cast_or_null<Instruction>(src_value);

  if (usesValueAsPointerOperand(src_value, dst_inst))
    return false;

  if (const auto *src_gep = dyn_cast_or_null<GetElementPtrInst>(src_value)) {
    if (usesValueAsPointerOperand(src_gep, dst_inst))
      return false;
  }

  if (const auto *dst_store = dyn_cast_or_null<StoreInst>(dst_value))
    return dst_store->getValueOperand() == src_value;

  if (const auto *dst_load = dyn_cast_or_null<LoadInst>(dst_value))
    return dst_load->getPointerOperand() != src_value;

  if (const auto *dst_gep = dyn_cast_or_null<GetElementPtrInst>(dst_value))
    return dst_gep->getPointerOperand() != src_value;

  if (const auto *src_store = dyn_cast_or_null<StoreInst>(src_value)) {
    if (dst_value != nullptr && src_store->getPointerOperand() == dst_value)
      return false;
  }

  return src_inst != nullptr || dst_inst != nullptr;
}

static bool applyCallStackTransition(bool forward, Node *current, Node *neighbor,
                                     EdgeType edge_type,
                                     std::vector<Node *> &call_stack) {
  if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
    if (forward) {
      call_stack.push_back(current);
      return true;
    }
    if (call_stack.empty() || neighbor != call_stack.back())
      return false;
    call_stack.pop_back();
    return true;
  }

  if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
    if (forward) {
      if (call_stack.empty() || neighbor != call_stack.back())
        return false;
      call_stack.pop_back();
      return true;
    }
    call_stack.push_back(current);
    return true;
  }

  return true;
}

static std::string pointerKey(const void *value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

static std::string locationKey(const DebugLoc &debug_loc) {
  if (!debug_loc)
    return "";
  std::string key = debug_loc->getFilename().str();
  key += ":";
  key += std::to_string(debug_loc.getLine());
  key += ":";
  key += std::to_string(debug_loc.getCol());
  return key;
}

static bool sourceLocationMatches(const PDGSourceLocation &wanted,
                                  const Instruction &inst) {
  DebugLoc loc = inst.getDebugLoc();
  if (!loc)
    return false;

  if (wanted.line != 0 && loc.getLine() != wanted.line)
    return false;
  if (wanted.column != 0 && loc.getCol() != wanted.column)
    return false;

  std::string file = loc->getFilename().str();
  if (wanted.file.empty())
    return true;
  if (file == wanted.file)
    return true;

  StringRef file_ref(file);
  StringRef wanted_ref(wanted.file);
  return file_ref.endswith(wanted_ref);
}

static NodeSet allGraphNodes(ProgramGraph &pdg) {
  NodeSet nodes;
  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it)
    if (*it != nullptr)
      nodes.insert(*it);
  return nodes;
}

static NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope) {
  NodeSet nodes;
  if (scope.kind == PDGQueryScope::Kind::WholeGraph)
    return allGraphNodes(pdg);

  if (scope.kind == PDGQueryScope::Kind::NodeSet)
    return scope.nodes;

  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    return scope.query_result->nodes;

  if (scope.kind == PDGQueryScope::Kind::Function && scope.function) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
      Node *node = *it;
      if (node == nullptr)
        continue;
      if (node->getFunc() == scope.function)
        nodes.insert(node);
    }
    if (pdg.hasNode(const_cast<Function &>(*scope.function)))
      nodes.insert(pdg.getNode(const_cast<Function &>(*scope.function)));
  }
  return nodes;
}

static EdgeSet collectInducedEdges(const NodeSet &nodes,
                                   const std::set<EdgeType> &edge_types) {
  EdgeSet edges;
  for (Node *node : nodes) {
    if (node == nullptr)
      continue;
    for (Node::EdgeSet::const_iterator it = node->getOutEdgeSet().begin();
         it != node->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      if (nodes.count(edge->getDstNode()) == 0)
        continue;
      edges.insert(edge);
    }
  }
  return edges;
}

static std::string scopeCacheKey(const PDGQueryScope &scope) {
  std::ostringstream os;
  os << static_cast<int>(scope.kind) << ":";
  if (scope.kind == PDGQueryScope::Kind::Function && scope.function)
    os << scope.function->getName().str();
  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    os << pointerKey(scope.query_result);
  if (scope.kind == PDGQueryScope::Kind::NodeSet) {
    for (Node *node : scope.nodes)
      os << pointerKey(node) << ",";
  }
  return os.str();
}

static std::string criteriaCacheKey(const PDGCriteria &criteria,
                                    const llvm::Module *module) {
  std::ostringstream os;
  os << pointerKey(module) << "|";
  for (Node *node : criteria.nodes)
    os << "n:" << pointerKey(node) << ";";
  for (Value *value : criteria.values)
    os << "v:" << pointerKey(value) << ";";
  for (size_t i = 0; i < criteria.function_names.size(); ++i)
    os << "f:" << criteria.function_names[i] << ";";
  for (size_t i = 0; i < criteria.callee_names.size(); ++i)
    os << "c:" << criteria.callee_names[i] << ";";
  for (size_t i = 0; i < criteria.source_locations.size(); ++i) {
    os << "l:" << criteria.source_locations[i].file << ":"
       << criteria.source_locations[i].line << ":"
       << criteria.source_locations[i].column << ";";
  }
  for (size_t i = 0; i < criteria.property_specs.size(); ++i)
    os << "p:" << static_cast<int>(criteria.property_specs[i].getType()) << ":"
       << criteria.property_specs[i].rules().size() << ";";
  for (size_t i = 0; i < criteria.cypher_selections.size(); ++i)
    os << "q:" << criteria.cypher_selections[i].query << ":"
       << criteria.cypher_selections[i].binding << ";";
  return os.str();
}

static std::string optionsCacheKey(const PDGQueryOptions &options) {
  std::ostringstream os;
  os << static_cast<int>(options.edge_preset) << "|"
     << static_cast<int>(options.context_mode) << "|"
     << static_cast<int>(options.cache_policy) << "|"
     << static_cast<int>(options.slice_flavor) << "|"
     << options.explain << "|" << scopeCacheKey(options.scope) << "|"
     << options.limits.max_depth << "|" << options.limits.max_states << "|"
     << options.limits.max_paths << "|" << options.limits.max_path_length
     << "|" << options.limits.max_stack_depth;
  return os.str();
}

static std::string pathKey(const PDGWitnessPath &path) {
  std::ostringstream os;
  os << static_cast<int>(path.kind) << "|";
  for (Node *node : path.nodes)
    os << pointerKey(node) << ",";
  return os.str();
}

static std::string functionNameForNode(Node *node) {
  if (node == nullptr)
    return "";
  if (node->getFunc() != nullptr)
    return node->getFunc()->getName().str();
  const Function *function = dyn_cast_or_null<Function>(node->getValue());
  return function ? function->getName().str() : "";
}

static std::string sourceKeyForNode(Node *node) {
  if (node == nullptr)
    return "";
  const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
  if (inst == nullptr || !inst->getDebugLoc())
    return "";
  return locationKey(inst->getDebugLoc());
}

static std::string stringifyValue(const Value *value) {
  if (value == nullptr)
    return "";
  std::string rendered;
  raw_string_ostream os(rendered);
  os << *value;
  return os.str();
}

static bool moduleHasAnyCall(const Module &module,
                             const std::vector<std::string> &names) {
  const std::vector<std::string> lowered_names = [&names]() {
    std::vector<std::string> result;
    for (size_t i = 0; i < names.size(); ++i)
      result.push_back(toLower(names[i]));
    return result;
  }();

  for (const Function &function : module) {
    if (function.isDeclaration())
      continue;
    for (const BasicBlock &block : function) {
      for (const Instruction &inst : block) {
        const CallBase *call = dyn_cast<CallBase>(&inst);
        if (call == nullptr || call->getCalledFunction() == nullptr)
          continue;
        const std::string callee =
            toLower(call->getCalledFunction()->getName().str());
        if (std::find(lowered_names.begin(), lowered_names.end(), callee) !=
            lowered_names.end())
          return true;
      }
    }
  }
  return false;
}

static bool isCallToAny(const Instruction &inst,
                        const std::vector<std::string> &names) {
  const CallBase *call = dyn_cast<CallBase>(&inst);
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return false;
  const std::string callee = toLower(call->getCalledFunction()->getName().str());
  for (size_t i = 0; i < names.size(); ++i) {
    if (callee == toLower(names[i]))
      return true;
  }
  return false;
}

static bool isSignedArithInstruction(const Instruction &inst) {
  switch (inst.getOpcode()) {
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::SDiv:
  case Instruction::SRem:
  case Instruction::Shl:
    return inst.getType()->isIntegerTy();
  default:
    return false;
  }
}

static bool isNullDerefInstruction(const Instruction &inst) {
  if (isa<LoadInst>(&inst) || isa<StoreInst>(&inst))
    return true;
  if (const CallBase *call = dyn_cast<CallBase>(&inst)) {
    for (unsigned i = 0; i < call->arg_size(); ++i) {
      if (call->getArgOperand(i)->getType()->isPointerTy())
        return true;
    }
  }
  return false;
}

static bool isMemSafetyInstruction(const Instruction &inst) {
  return isa<LoadInst>(&inst) || isa<StoreInst>(&inst) ||
         isa<MemIntrinsic>(&inst) || isa<AllocaInst>(&inst) ||
         isa<CallBase>(&inst);
}

static bool isDefBehaviorInstruction(const Instruction &inst) {
  return isSignedArithInstruction(inst) || isa<LoadInst>(&inst) ||
         isa<StoreInst>(&inst) || isa<CallBase>(&inst);
}

static NodeSet resolvePropertyCriteria(ProgramGraph &pdg, const Module &module,
                                       const PropertySpec &spec) {
  NodeSet criteria;

  auto addInstructionNode = [&](const Instruction &inst) {
    Node *node = pdg.getNode(const_cast<Instruction &>(inst));
    if (node != nullptr)
      criteria.insert(node);
  };

  for (size_t rule_index = 0; rule_index < spec.rules().size(); ++rule_index) {
    const PropertyRule &rule = spec.rules()[rule_index];
    if (rule.kind == PropertyKind::UnreachCall ||
        rule.kind == PropertyKind::Assertions ||
        rule.kind == PropertyKind::CoverageErrorCall) {
      std::vector<std::string> targets;
      if (rule.kind == PropertyKind::Assertions) {
        targets.push_back("__assert_fail");
        targets.push_back("__verifier_error");
      } else if (rule.target.empty()) {
        targets.push_back("reach_error");
        if (rule.kind == PropertyKind::UnreachCall)
          targets.push_back("__verifier_error");
      } else {
        const std::string target = toLower(rule.target);
        targets.push_back(target);
        if (target == "reach_error")
          targets.push_back("__verifier_error");
      }

      for (const Function &function : module) {
        if (function.isDeclaration())
          continue;
        for (const BasicBlock &block : function) {
          for (const Instruction &inst : block) {
            if (isCallToAny(inst, targets))
              addInstructionNode(inst);
          }
        }
      }
      continue;
    }

    const bool has_mem_markers =
        (rule.kind == PropertyKind::MemSafety ||
         rule.kind == PropertyKind::MemCleanup) &&
        moduleHasAnyCall(module, std::vector<std::string>{
                                     "__INSTR_mark_pointer",
                                     "__INSTR_mark_free",
                                     "__INSTR_mark_allocation",
                                     "__INSTR_mark_exit"});

    const bool has_overflow_markers =
        (rule.kind == PropertyKind::NoOverflow ||
         rule.kind == PropertyKind::DefBehavior) &&
        moduleHasAnyCall(module, std::vector<std::string>{
                                     "__VERIFIER_error",
                                     "__symbiotic_check_overflow"});

    const bool has_null_markers =
        rule.kind == PropertyKind::NullDeref &&
        moduleHasAnyCall(module,
                         std::vector<std::string>{"__INSTR_mark_pointer"});

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const BasicBlock &block : function) {
        for (const Instruction &inst : block) {
          switch (rule.kind) {
          case PropertyKind::MemSafety:
          case PropertyKind::MemCleanup:
            if (has_mem_markers) {
              if (isCallToAny(inst, std::vector<std::string>{
                                        "__INSTR_mark_pointer",
                                        "__INSTR_mark_free",
                                        "__INSTR_mark_allocation",
                                        "__INSTR_mark_exit"}))
                addInstructionNode(inst);
            } else if (isMemSafetyInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::NoOverflow:
            if (has_overflow_markers) {
              if (isCallToAny(inst, std::vector<std::string>{
                                        "__VERIFIER_error",
                                        "__symbiotic_check_overflow"}))
                addInstructionNode(inst);
            } else if (isSignedArithInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::DefBehavior:
            if (has_overflow_markers) {
              if (isCallToAny(inst, std::vector<std::string>{
                                        "__VERIFIER_error",
                                        "__symbiotic_check_overflow"}))
                addInstructionNode(inst);
            } else if (isDefBehaviorInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          case PropertyKind::Termination:
            if (isCallToAny(inst, std::vector<std::string>{
                                      "__INSTR_fail", "__assert_fail",
                                      "__VERIFIER_silent_exit",
                                      "__VERIFIER_exit",
                                      "__INSTR_check_assume"}))
              addInstructionNode(inst);
            break;
          case PropertyKind::NullDeref:
            if (has_null_markers) {
              if (isCallToAny(inst,
                              std::vector<std::string>{"__INSTR_mark_pointer"})) {
                if (const Instruction *next = inst.getNextNode())
                  addInstructionNode(*next);
                else
                  addInstructionNode(inst);
              }
            } else if (isNullDerefInstruction(inst)) {
              addInstructionNode(inst);
            }
            break;
          default:
            break;
          }
        }
      }
    }
  }

  return criteria;
}

static bool shouldRecordNode(const NodeSet &scope, Node *node) {
  return scope.empty() || scope.count(node) != 0;
}

static TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                                      const std::set<EdgeType> &edge_types,
                                      const PDGQueryOptions &options,
                                      bool forward,
                                      PDGQueryDiagnostics &diagnostics) {
  TraversalOutcome outcome;
  const NodeSet scoped_nodes = scopeNodes(pdg, options.scope);
  std::queue<TraversalState> worklist;
  std::unordered_set<Node *> visited_nodes;
  std::unordered_set<std::pair<Node *, std::vector<Node *>>, StateHash>
      visited_states;

  for (Node *node : start_nodes) {
    if (node == nullptr)
      continue;
    if (!shouldRecordNode(scoped_nodes, node))
      diagnostics.unresolved_criteria.push_back("criteria node outside scope");
    outcome.nodes.insert(node);
    outcome.distances[node] = 0;
    worklist.push(TraversalState{node, 0, std::vector<Node *>()});
  }

  while (!worklist.empty()) {
    const TraversalState state = worklist.front();
    worklist.pop();

    diagnostics.explored_states++;
    diagnostics.max_depth_reached =
        std::max(diagnostics.max_depth_reached, state.depth);
    diagnostics.max_stack_depth_reached =
        std::max(diagnostics.max_stack_depth_reached, state.call_stack.size());

    if (options.limits.max_states > 0 &&
        diagnostics.explored_states > options.limits.max_states) {
      diagnostics.state_limit_hit = true;
      break;
    }

    if (options.context_mode == PDGContextMode::ContextSensitive) {
      const std::pair<Node *, std::vector<Node *>> key(state.node,
                                                       state.call_stack);
      if (visited_states.count(key) != 0)
        continue;
      visited_states.insert(key);
    } else {
      if (visited_nodes.count(state.node) != 0)
        continue;
      visited_nodes.insert(state.node);
    }

    if (options.limits.max_depth > 0 && state.depth >= options.limits.max_depth) {
      diagnostics.depth_limit_hit = true;
      continue;
    }

    const bool current_is_field_access = isFieldAccess(state.node);
    const Node::EdgeSet &edges =
        forward ? state.node->getOutEdgeSet() : state.node->getInEdgeSet();

    for (Node::EdgeSet::const_iterator it = edges.begin(); it != edges.end();
         ++it) {
      Edge *edge = *it;
      if (edge == nullptr)
        continue;

      Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
      if (neighbor == nullptr)
        continue;

      const EdgeType edge_type = edge->getEdgeType();
      if (!isEdgeAllowed(edge_type, edge_types))
        continue;

      std::vector<Node *> next_stack = state.call_stack;
      if (options.context_mode == PDGContextMode::ContextSensitive &&
          !applyCallStackTransition(forward, state.node, neighbor, edge_type,
                                    next_stack)) {
        continue;
      }

      if (options.limits.max_stack_depth > 0 &&
          next_stack.size() > options.limits.max_stack_depth) {
        diagnostics.stack_depth_limit_hit = true;
        continue;
      }

      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      if (options.slice_flavor == SliceFlavor::Thin) {
        if (isControlEdge(edge_type))
          continue;
        if ((current_is_field_access && !forward &&
             !isValueFlowEdge(edge, neighbor, state.node)) ||
            (isFieldAccess(neighbor) && forward &&
             !isValueFlowEdge(edge, state.node, neighbor))) {
          continue;
        }
      }

      outcome.nodes.insert(neighbor);
      outcome.edges.insert(edge);
      outcome.predecessors[neighbor].insert(state.node);
      outcome.predecessor_edges[neighbor].push_back(
          std::make_pair(state.node, edge_type));

      const size_t next_depth = state.depth + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          outcome.distances.find(neighbor);
      if (dist_it == outcome.distances.end() || next_depth < dist_it->second)
        outcome.distances[neighbor] = next_depth;

      TraversalState next_state;
      next_state.node = neighbor;
      next_state.depth = next_depth;
      next_state.call_stack = next_stack;
      worklist.push(next_state);
    }
  }

  outcome.edges = collectInducedEdges(outcome.nodes, edge_types);
  return outcome;
}

static std::vector<Node *> materializePath(
    Node *node,
    const std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>> &
        predecessor_edges,
    const NodeSet &start_nodes, std::vector<EdgeType> *edge_types) {
  std::vector<Node *> path;
  std::vector<EdgeType> local_edge_types;
  Node *current = node;
  std::unordered_set<Node *> seen;

  while (current != nullptr && seen.insert(current).second) {
    path.push_back(current);
    if (start_nodes.count(current) != 0)
      break;
    std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>::const_iterator
        it = predecessor_edges.find(current);
    if (it == predecessor_edges.end() || it->second.empty())
      break;
    local_edge_types.push_back(it->second.front().second);
    current = it->second.front().first;
  }

  std::reverse(path.begin(), path.end());
  std::reverse(local_edge_types.begin(), local_edge_types.end());
  if (edge_types != nullptr)
    *edge_types = local_edge_types;
  return path;
}

static void fillWitnessPaths(PDGQueryResult &result) {
  if (result.criteria_nodes.empty())
    return;

  std::set<std::string> seen_paths;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
      predecessor_edges;
  for (std::unordered_map<Node *, std::set<Node *>>::const_iterator it =
           result.predecessors.begin();
       it != result.predecessors.end(); ++it) {
    for (std::set<Node *>::const_iterator pred_it = it->second.begin();
         pred_it != it->second.end(); ++pred_it) {
      predecessor_edges[it->first].push_back(
          std::make_pair(*pred_it, EdgeType::TYPE_OTHEREDGE));
    }
  }

  for (NodeSet::const_iterator it = result.nodes.begin(); it != result.nodes.end();
       ++it) {
    Node *node = *it;
    if (node == nullptr || result.criteria_nodes.count(node) != 0)
      continue;
    PDGWitnessPath witness;
    witness.kind = PDGWitnessPathKind::Slice;
    witness.nodes = materializePath(node, predecessor_edges,
                                    result.criteria_nodes, &witness.edge_types);
    if (witness.nodes.size() < 2)
      continue;
    const std::string key = pathKey(witness);
    if (seen_paths.insert(key).second)
      result.witness_paths.push_back(witness);
  }
}

static PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
                                          const TraversalOutcome &outcome,
                                          PDGQueryDiagnostics diagnostics,
                                          bool explain) {
  PDGQueryResult result;
  result.criteria_nodes = criteria_nodes;
  result.nodes = outcome.nodes;
  result.edges = outcome.edges;
  result.predecessors = outcome.predecessors;
  result.distances = outcome.distances;
  result.diagnostics = diagnostics;
  if (explain)
    fillWitnessPaths(result);
  return result;
}

static void syncCacheEpoch(ProgramGraph &pdg, unsigned long long &cache_epoch,
                           std::unordered_map<std::string, PDGQueryResult> &result_cache,
                           std::unordered_map<std::string, NodeSet> *criteria_cache,
                           std::unordered_map<std::string,
                                              std::unordered_map<Node *, std::set<Node *>>> *closure_cache) {
  if (cache_epoch == pdg.getEpoch())
    return;
  cache_epoch = pdg.getEpoch();
  result_cache.clear();
  if (criteria_cache != nullptr)
    criteria_cache->clear();
  if (closure_cache != nullptr)
    closure_cache->clear();
}

static EdgeType edgeBetween(Node *from, Node *to) {
  if (from == nullptr || to == nullptr)
    return EdgeType::TYPE_OTHEREDGE;
  for (Node::EdgeSet::const_iterator it = from->getOutEdgeSet().begin();
       it != from->getOutEdgeSet().end(); ++it) {
    Edge *edge = *it;
    if (edge != nullptr && edge->getDstNode() == to)
      return edge->getEdgeType();
  }
  return EdgeType::TYPE_OTHEREDGE;
}

static bool isGlobalNode(Node *node) {
  if (node == nullptr)
    return false;
  const GraphNodeType type = node->getNodeType();
  if (node->getValue() == nullptr) {
    return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
           type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
           type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
  }
  return type == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
         type == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
         type == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE ||
         isa<GlobalValue>(node->getValue());
}

static bool isInputNode(Node *node, const Function &function) {
  if (node == nullptr)
    return false;
  if (node->getNodeType() == GraphNodeType::PARAM_FORMALIN ||
      node->getNodeType() == GraphNodeType::PARAM_ACTUALIN)
    return true;
  const Value *value = node->getValue();
  const Argument *argument = dyn_cast_or_null<Argument>(value);
  return argument != nullptr && argument->getParent() == &function;
}

static bool isReturnNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_RET;
}

static bool isCallNode(Node *node) {
  return node != nullptr && node->getNodeType() == GraphNodeType::INST_FUNCALL;
}

static bool isControlPredicateNode(Node *node) {
  return node != nullptr &&
         (node->getNodeType() == GraphNodeType::INST_BR ||
          node->getNodeType() == GraphNodeType::FUNC_ENTRY);
}

static const Function *functionForNode(Node *node) {
  if (node == nullptr)
    return nullptr;
  if (node->getFunc() != nullptr)
    return node->getFunc();
  const Value *value = node->getValue();
  if (const Argument *argument = dyn_cast_or_null<Argument>(value))
    return argument->getParent();
  return dyn_cast_or_null<Function>(value);
}

static NodeSet collectFunctionNodes(ProgramGraph &pdg, const Function &function,
                                    bool include_connected_globals) {
  NodeSet nodes;
  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
    Node *node = *it;
    if (node == nullptr)
      continue;
    if (functionForNode(node) == &function)
      nodes.insert(node);
  }
  if (pdg.hasNode(const_cast<Function &>(function)))
    nodes.insert(pdg.getNode(const_cast<Function &>(function)));

  if (!include_connected_globals)
    return nodes;

  for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
    Node *node = *it;
    if (!isGlobalNode(node))
      continue;
    bool connected = false;
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end() && !connected; ++edge_it) {
      Edge *edge = *edge_it;
      connected = edge != nullptr && functionForNode(edge->getDstNode()) == &function;
    }
    for (Node::EdgeSet::const_iterator edge_it = node->getInEdgeSet().begin();
         edge_it != node->getInEdgeSet().end() && !connected; ++edge_it) {
      Edge *edge = *edge_it;
      connected = edge != nullptr && functionForNode(edge->getSrcNode()) == &function;
    }
    if (connected)
      nodes.insert(node);
  }

  return nodes;
}

static std::string functionSummaryCacheKey(const Function &function,
                                           const SummaryPolicy &policy,
                                           const PDGQueryOptions &options) {
  std::ostringstream os;
  os << function.getName().str() << "|"
     << static_cast<int>(policy.kind) << "|"
     << policy.max_witnesses_per_bucket << "|" << optionsCacheKey(options);
  return os.str();
}

static bool tryResolveSingleFunction(const ProgramGraph &pdg,
                                     const PDGCriteria &criteria,
                                     const PDGQueryOptions &options,
                                     const Module *module,
                                     const Function *&function,
                                     PDGQueryDiagnostics &diagnostics) {
  function = nullptr;
  if (options.scope.kind == PDGQueryScope::Kind::Function &&
      options.scope.function != nullptr) {
    function = options.scope.function;
    return true;
  }

  if (criteria.function_names.size() == 1 && module != nullptr) {
    function = module->getFunction(criteria.function_names.front());
    if (function != nullptr)
      return true;
  }

  PDGCriteriaResolver resolver(const_cast<ProgramGraph &>(pdg));
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  std::set<const Function *> functions;
  for (NodeSet::const_iterator it = resolved.nodes.begin(); it != resolved.nodes.end();
       ++it) {
    const Function *candidate = functionForNode(*it);
    if (candidate != nullptr)
      functions.insert(candidate);
  }
  if (functions.size() == 1) {
    function = *functions.begin();
    return true;
  }
  if (functions.empty())
    diagnostics.unresolved_criteria.push_back(
        "summary query did not resolve to any function");
  else
    diagnostics.unresolved_criteria.push_back(
        "summary query resolved to multiple functions");
  return false;
}

static size_t countInterproceduralCrossings(const std::vector<EdgeType> &edges) {
  size_t count = 0;
  for (size_t i = 0; i < edges.size(); ++i) {
    const EdgeType type = edges[i];
    if (type == EdgeType::CONTROLDEP_CALLINV ||
        type == EdgeType::CONTROLDEP_CALLRET || isParameterEdge(type) ||
        type == EdgeType::DATA_RET)
      ++count;
  }
  return count;
}

static std::string calleeName(Node *node) {
  if (!isCallNode(node))
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

static ResourceKind resourceKindForAcquireName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "malloc" || lower == "calloc" || lower == "realloc")
    return ResourceKind::Heap;
  if (lower == "fopen")
    return ResourceKind::File;
  if (lower == "open" || lower == "socket")
    return ResourceKind::FileDescriptor;
  if (lower == "opendir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForReleaseName(const std::string &api_name) {
  const std::string lower = toLower(api_name);
  if (lower == "free")
    return ResourceKind::Heap;
  if (lower == "fclose")
    return ResourceKind::File;
  if (lower == "close")
    return ResourceKind::FileDescriptor;
  if (lower == "closedir")
    return ResourceKind::Directory;
  return ResourceKind::Unknown;
}

static ResourceKind resourceKindForNode(Node *node, bool release) {
  const std::string name = calleeName(node);
  return release ? resourceKindForReleaseName(name)
                 : resourceKindForAcquireName(name);
}

static bool resourceKindMatches(ResourceKind lhs, ResourceKind filter) {
  return filter == ResourceKind::Unknown || lhs == filter;
}

static bool summaryKindEnabled(SummaryKind selected, SummaryKind candidate) {
  return selected == SummaryKind::All || selected == candidate;
}

static bool containsBucketEntry(const std::vector<SummaryBucketEntry> &entries,
                                Node *source, Node *target) {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].source == source && entries[i].target == target)
      return true;
  }
  return false;
}

static void appendBucketEntry(std::vector<SummaryBucketEntry> &entries,
                              Node *source, Node *target,
                              const std::vector<PDGWitnessPath> &witnesses) {
  if (containsBucketEntry(entries, source, target))
    return;
  SummaryBucketEntry entry;
  entry.source = source;
  entry.target = target;
  entry.witness_paths = witnesses;
  entries.push_back(entry);
}

} // namespace

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset) {
  switch (preset) {
  case PDGEdgePreset::All:
    return {EdgeType::IND_CALL,
            EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY,
            EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR,
            EdgeType::DATA_DEF_USE,
            EdgeType::DATA_RAW,
            EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,
            EdgeType::VAL_DEP,
            EdgeType::CLS_MTH,
            EdgeType::ANNO_VAR,
            EdgeType::ANNO_GLOBAL,
            EdgeType::ANNO_OTHER,
            EdgeType::TYPE_OTHEREDGE};
  case PDGEdgePreset::Data:
    return {EdgeType::DATA_DEF_USE, EdgeType::DATA_RAW, EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,   EdgeType::DATA_RET, EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT, EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,   EdgeType::VAL_DEP};
  case PDGEdgePreset::Control:
    return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR};
  case PDGEdgePreset::Parameter:
    return {EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::Interprocedural:
    return {EdgeType::IND_CALL, EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET, EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::ValueFlow:
    return {EdgeType::DATA_DEF_USE, EdgeType::DATA_RAW, EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS, EdgeType::DATA_RET, EdgeType::VAL_DEP,
            EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD, EdgeType::GLOBAL_DEP};
  case PDGEdgePreset::TransformLegality:
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
  return std::set<EdgeType>();
}

std::string describeNode(Node *node) {
  if (node == nullptr)
    return "<null>";
  std::string description = pdgutils::getNodeTypeStr(node->getNodeType());
  if (node->getValue() != nullptr) {
    description += " ";
    description += stringifyValue(node->getValue());
  }
  return description;
}

std::string stableNodeKey(Node *node) {
  if (node == nullptr)
    return "<null>";

  std::ostringstream os;
  os << pdgutils::getNodeTypeStr(node->getNodeType()) << "|"
     << functionNameForNode(node) << "|";
  const std::string source = sourceKeyForNode(node);
  if (!source.empty())
    os << source;
  else if (node->getValue() != nullptr)
    os << stringifyValue(node->getValue());
  else
    os << pointerKey(node);
  return os.str();
}

std::string resourceKindName(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::Heap:
    return "heap";
  case ResourceKind::File:
    return "file";
  case ResourceKind::FileDescriptor:
    return "fd";
  case ResourceKind::Directory:
    return "dir";
  case ResourceKind::Unknown:
  default:
    return "unknown";
  }
}

PDGQueryResult
PDGCriteriaResolver::resolve(const PDGCriteria &criteria,
                             const PDGQueryOptions &options,
                             const Module *module) const {
  PDGQueryResult result;

  for (NodeSet::const_iterator it = criteria.nodes.begin();
       it != criteria.nodes.end(); ++it) {
    if (*it != nullptr)
      result.nodes.insert(*it);
  }

  for (size_t i = 0; i < criteria.values.size(); ++i) {
    Value *value = criteria.values[i];
    if (value == nullptr)
      continue;
    if (Node *node = pdg_.getNode(*value))
      result.nodes.insert(node);
    else
      result.diagnostics.unresolved_criteria.push_back(
          "value has no PDG node");
  }

  if (!criteria.function_names.empty()) {
    for (ProgramGraph::FuncWrapperMap::iterator it =
             pdg_.getFuncWrapperMap().begin();
         it != pdg_.getFuncWrapperMap().end(); ++it) {
      Function *function = it->first;
      if (function == nullptr)
        continue;
      const std::string function_name = toLower(function->getName().str());
      for (size_t name_index = 0; name_index < criteria.function_names.size();
           ++name_index) {
        if (function_name == toLower(criteria.function_names[name_index])) {
          if (Node *entry = pdg_.getNode(*function))
            result.nodes.insert(entry);
        }
      }
    }
  }

  if (!criteria.callee_names.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "callee criteria requires module");
    } else {
      for (const Function &function : *module) {
        if (function.isDeclaration())
          continue;
        for (const BasicBlock &block : function) {
          for (const Instruction &inst : block) {
            const CallBase *call = dyn_cast<CallBase>(&inst);
            if (call == nullptr || call->getCalledFunction() == nullptr)
              continue;
            const std::string callee =
                toLower(call->getCalledFunction()->getName().str());
            for (size_t i = 0; i < criteria.callee_names.size(); ++i) {
              if (callee == toLower(criteria.callee_names[i])) {
                if (Node *node = pdg_.getNode(const_cast<Instruction &>(inst)))
                  result.nodes.insert(node);
              }
            }
          }
        }
      }
    }
  }

  if (!criteria.source_locations.empty()) {
    for (ProgramGraph::NodeSet::iterator it = pdg_.begin(); it != pdg_.end(); ++it) {
      Node *node = *it;
      const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
      if (inst == nullptr)
        continue;
      for (size_t location_index = 0;
           location_index < criteria.source_locations.size(); ++location_index) {
        if (sourceLocationMatches(criteria.source_locations[location_index],
                                  *inst)) {
          result.nodes.insert(node);
          break;
        }
      }
    }
  }

  if (!criteria.property_specs.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "property criteria requires module");
    } else {
      for (size_t i = 0; i < criteria.property_specs.size(); ++i) {
        NodeSet property_nodes =
            resolvePropertyCriteria(pdg_, *module, criteria.property_specs[i]);
        result.nodes.insert(property_nodes.begin(), property_nodes.end());
      }
    }
  }

  for (size_t i = 0; i < criteria.cypher_selections.size(); ++i) {
    const CypherSelection &selection = criteria.cypher_selections[i];
    CypherParser parser;
    std::unique_ptr<CypherQuery> query = parser.parse(selection.query);
    if (!query) {
      result.diagnostics.unresolved_criteria.push_back(parser.getLastError().message);
      continue;
    }

    CypherQueryExecutor executor(pdg_);
    std::unique_ptr<CypherResult> cypher_result = executor.execute(*query);
    if (!cypher_result) {
      result.diagnostics.unresolved_criteria.push_back(executor.getLastError());
      continue;
    }

    if (!selection.binding.empty()) {
      const std::vector<Node *> *bound = executor.getBoundVariable(selection.binding);
      if (bound == nullptr) {
        result.diagnostics.unresolved_criteria.push_back(
            "missing Cypher binding: " + selection.binding);
        continue;
      }
      result.nodes.insert(bound->begin(), bound->end());
    } else {
      result.nodes.insert(cypher_result->getNodes().begin(),
                          cypher_result->getNodes().end());
    }
  }

  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  if (!scoped_nodes.empty()) {
    NodeSet filtered;
    for (NodeSet::const_iterator it = result.nodes.begin(); it != result.nodes.end();
         ++it) {
      if (scoped_nodes.count(*it) != 0)
        filtered.insert(*it);
    }
    if (filtered.empty() && !result.nodes.empty())
      result.diagnostics.unresolved_criteria.push_back(
          "all resolved criteria were excluded by scope");
    result.nodes.swap(filtered);
  }

  result.criteria_nodes = result.nodes;
  return result;
}

SliceQuery::SliceQuery(ProgramGraph &pdg) : pdg_(pdg) {}

PDGQueryResult SliceQuery::forward(const PDGCriteria &criteria,
                                   const PDGQueryOptions &options,
                                   const Module *module) const {
  syncCacheEpoch(pdg_, cache_epoch_, result_cache_, &criteria_cache_, nullptr);
  const std::string criteria_key = criteriaCacheKey(criteria, module);
  const std::string cache_key =
      "forward|" + criteria_key + "|" + optionsCacheKey(options);

  if (options.cache_policy == PDGCachePolicy::Enabled) {
    std::unordered_map<std::string, PDGQueryResult>::const_iterator cached =
        result_cache_.find(cache_key);
    if (cached != result_cache_.end()) {
      PDGQueryResult result = cached->second;
      result.diagnostics.summary_cache_hits++;
      return result;
    }
  }

  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  NodeSet criteria_nodes = resolved.nodes;
  TraversalOutcome outcome =
      traverseGraph(pdg_, criteria_nodes, edgeTypesForPreset(options.edge_preset),
                    options, true, resolved.diagnostics);
  PDGQueryResult result =
      resultFromTraversal(criteria_nodes, outcome, resolved.diagnostics,
                          options.explain);

  if (options.cache_policy == PDGCachePolicy::Enabled)
    result_cache_[cache_key] = result;
  return result;
}

PDGQueryResult SliceQuery::backward(const PDGCriteria &criteria,
                                    const PDGQueryOptions &options,
                                    const Module *module) const {
  syncCacheEpoch(pdg_, cache_epoch_, result_cache_, &criteria_cache_, nullptr);
  const std::string criteria_key = criteriaCacheKey(criteria, module);
  const std::string cache_key =
      "backward|" + criteria_key + "|" + optionsCacheKey(options);

  if (options.cache_policy == PDGCachePolicy::Enabled) {
    std::unordered_map<std::string, PDGQueryResult>::const_iterator cached =
        result_cache_.find(cache_key);
    if (cached != result_cache_.end()) {
      PDGQueryResult result = cached->second;
      result.diagnostics.summary_cache_hits++;
      return result;
    }
  }

  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  NodeSet criteria_nodes = resolved.nodes;
  TraversalOutcome outcome =
      traverseGraph(pdg_, criteria_nodes, edgeTypesForPreset(options.edge_preset),
                    options, false, resolved.diagnostics);
  PDGQueryResult result =
      resultFromTraversal(criteria_nodes, outcome, resolved.diagnostics,
                          options.explain);

  if (options.cache_policy == PDGCachePolicy::Enabled)
    result_cache_[cache_key] = result;
  return result;
}

PDGQueryResult SliceQuery::chop(const PDGCriteria &sources,
                                const PDGCriteria &targets,
                                const PDGQueryOptions &options,
                                const Module *module) const {
  PDGQueryResult source_slice = forward(sources, options, module);
  PDGQueryResult target_slice = backward(targets, options, module);

  PDGQueryResult result;
  result.criteria_nodes = source_slice.criteria_nodes;
  result.criteria_nodes.insert(target_slice.criteria_nodes.begin(),
                               target_slice.criteria_nodes.end());

  for (NodeSet::const_iterator it = source_slice.nodes.begin();
       it != source_slice.nodes.end(); ++it) {
    if (target_slice.nodes.count(*it) != 0)
      result.nodes.insert(*it);
  }

  result.edges =
      collectInducedEdges(result.nodes, edgeTypesForPreset(options.edge_preset));
  result.predecessors = source_slice.predecessors;
  result.distances = source_slice.distances;
  result.diagnostics = source_slice.diagnostics;
  if (options.explain) {
    DependenceQuery dep(pdg_);
    std::vector<PDGWitnessPath> paths =
        dep.allShortestPaths(sources, targets, options, module);
    for (size_t i = 0; i < paths.size(); ++i) {
      paths[i].kind = PDGWitnessPathKind::Chop;
      result.witness_paths.push_back(paths[i]);
    }
  }
  return result;
}

DependenceQuery::DependenceQuery(ProgramGraph &pdg) : pdg_(pdg) {}

PDGQueryResult DependenceQuery::reachability(const PDGCriteria &sources,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  SliceQuery slice(pdg_);
  return slice.forward(sources, options, module);
}

PDGQueryResult DependenceQuery::shortestPath(const PDGCriteria &sources,
                                             const PDGCriteria &targets,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult source_nodes = resolver.resolve(sources, options, module);
  PDGQueryResult target_nodes = resolver.resolve(targets, options, module);
  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);

  std::queue<Node *> worklist;
  std::unordered_map<Node *, size_t> distances;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>> preds;

  for (NodeSet::const_iterator it = source_nodes.nodes.begin();
       it != source_nodes.nodes.end(); ++it) {
    worklist.push(*it);
    distances[*it] = 0;
  }

  size_t best_distance = static_cast<size_t>(-1);
  Node *best_target = nullptr;

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    const size_t distance_to_current = distances[current];
    if (distance_to_current >= best_distance)
      continue;

    for (Node::EdgeSet::const_iterator it = current->getOutEdgeSet().begin();
         it != current->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr)
        continue;
      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      const size_t next_distance = distance_to_current + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          distances.find(neighbor);
      if (dist_it == distances.end() || next_distance < dist_it->second) {
        distances[neighbor] = next_distance;
        preds[neighbor].clear();
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
        worklist.push(neighbor);
      } else if (next_distance == dist_it->second) {
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
      }

      if (target_nodes.nodes.count(neighbor) != 0 &&
          next_distance < best_distance) {
        best_distance = next_distance;
        best_target = neighbor;
      }
    }
  }

  PDGQueryResult result;
  result.criteria_nodes = source_nodes.nodes;
  result.criteria_nodes.insert(target_nodes.nodes.begin(), target_nodes.nodes.end());
  result.diagnostics = source_nodes.diagnostics;
  result.diagnostics.unresolved_criteria.insert(
      result.diagnostics.unresolved_criteria.end(),
      target_nodes.diagnostics.unresolved_criteria.begin(),
      target_nodes.diagnostics.unresolved_criteria.end());

  if (best_target == nullptr)
    return result;

  Node *cursor = best_target;
  std::vector<Node *> path_nodes;
  std::vector<EdgeType> path_edges;
  std::unordered_set<Node *> seen;
  while (cursor != nullptr && seen.insert(cursor).second) {
    path_nodes.push_back(cursor);
    if (source_nodes.nodes.count(cursor) != 0)
      break;
    if (preds[cursor].empty())
      break;
    path_edges.push_back(preds[cursor].front().second);
    result.predecessors[cursor].insert(preds[cursor].front().first);
    cursor = preds[cursor].front().first;
  }
  std::reverse(path_nodes.begin(), path_nodes.end());
  std::reverse(path_edges.begin(), path_edges.end());

  result.nodes.insert(path_nodes.begin(), path_nodes.end());
  result.edges = collectInducedEdges(result.nodes, edge_types);
  result.distances[best_target] = best_distance;
  if (options.explain) {
    PDGWitnessPath witness;
    witness.kind = PDGWitnessPathKind::ShortestPath;
    witness.nodes = path_nodes;
    witness.edge_types = path_edges;
    result.witness_paths.push_back(witness);
  }
  return result;
}

std::vector<PDGWitnessPath>
DependenceQuery::allShortestPaths(const PDGCriteria &sources,
                                  const PDGCriteria &targets,
                                  const PDGQueryOptions &options,
                                  const Module *module) const {
  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult source_nodes = resolver.resolve(sources, options, module);
  PDGQueryResult target_nodes = resolver.resolve(targets, options, module);
  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);

  std::queue<Node *> worklist;
  std::unordered_map<Node *, size_t> distances;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>> preds;

  for (NodeSet::const_iterator it = source_nodes.nodes.begin();
       it != source_nodes.nodes.end(); ++it) {
    worklist.push(*it);
    distances[*it] = 0;
  }

  size_t best_distance = static_cast<size_t>(-1);
  std::vector<Node *> best_targets;

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    const size_t distance_to_current = distances[current];
    if (distance_to_current >= best_distance)
      continue;

    for (Node::EdgeSet::const_iterator it = current->getOutEdgeSet().begin();
         it != current->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr)
        continue;
      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      const size_t next_distance = distance_to_current + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          distances.find(neighbor);
      if (dist_it == distances.end()) {
        distances[neighbor] = next_distance;
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
        worklist.push(neighbor);
      } else if (next_distance == dist_it->second) {
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
      }

      if (target_nodes.nodes.count(neighbor) != 0) {
        if (next_distance < best_distance) {
          best_distance = next_distance;
          best_targets.clear();
          best_targets.push_back(neighbor);
        } else if (next_distance == best_distance) {
          best_targets.push_back(neighbor);
        }
      }
    }
  }

  std::vector<PDGWitnessPath> results;
  std::set<std::string> seen_paths;
  std::function<void(Node *, std::vector<Node *> &, std::vector<EdgeType> &)>
      build = [&](Node *current, std::vector<Node *> &nodes,
                  std::vector<EdgeType> &edges) {
        if (source_nodes.nodes.count(current) != 0) {
          PDGWitnessPath witness;
          witness.kind = PDGWitnessPathKind::AllShortestPath;
          witness.nodes.assign(nodes.rbegin(), nodes.rend());
          witness.edge_types.assign(edges.rbegin(), edges.rend());
          const std::string key = pathKey(witness);
          if (seen_paths.insert(key).second)
            results.push_back(witness);
          return;
        }

        std::vector<std::pair<Node *, EdgeType>> &pred_list = preds[current];
        for (size_t i = 0; i < pred_list.size(); ++i) {
          nodes.push_back(pred_list[i].first);
          edges.push_back(pred_list[i].second);
          build(pred_list[i].first, nodes, edges);
          edges.pop_back();
          nodes.pop_back();
          if (options.limits.max_paths > 0 &&
              results.size() >= options.limits.max_paths)
            return;
        }
      };

  for (size_t i = 0; i < best_targets.size(); ++i) {
    std::vector<Node *> nodes;
    std::vector<EdgeType> edges;
    nodes.push_back(best_targets[i]);
    build(best_targets[i], nodes, edges);
    if (options.limits.max_paths > 0 &&
        results.size() >= options.limits.max_paths)
      break;
  }

  return results;
}

size_t DependenceQuery::distance(const PDGCriteria &sources,
                                 const PDGCriteria &targets,
                                 const PDGQueryOptions &options,
                                 const Module *module) const {
  PDGQueryResult result = shortestPath(sources, targets, options, module);
  if (result.witness_paths.empty())
    return static_cast<size_t>(-1);
  return result.witness_paths.front().nodes.size() - 1;
}

PDGQueryResult DataFlowQuery::reachingDefinitions(const PDGCriteria &uses,
                                                  const PDGQueryOptions &options,
                                                  const Module *module) const {
  PDGQueryOptions data_options = options;
  data_options.edge_preset = PDGEdgePreset::Data;
  SliceQuery slice(pdg_);
  return slice.backward(uses, data_options, module);
}

std::vector<DefUseLink>
DataFlowQuery::defUseChain(Node &definition,
                           const PDGQueryOptions &options) const {
  std::vector<DefUseLink> chain;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::Data;
  PDGCriteria criteria;
  criteria.nodes.insert(&definition);
  PDGQueryResult result = reachingDefinitions(criteria, local_options, nullptr);
  for (NodeSet::const_iterator it = result.nodes.begin(); it != result.nodes.end();
       ++it) {
    Node *node = *it;
    if (node == &definition)
      continue;
    if (result.predecessors.count(node) == 0)
      continue;
    for (std::set<Node *>::const_iterator pred_it =
             result.predecessors[node].begin();
         pred_it != result.predecessors[node].end(); ++pred_it) {
      if (*pred_it == nullptr)
        continue;
      chain.push_back(DefUseLink{*pred_it, node, edgeBetween(*pred_it, node)});
    }
  }
  return chain;
}

std::vector<DefUseLink>
DataFlowQuery::useDefChain(Node &use, const PDGQueryOptions &options) const {
  std::vector<DefUseLink> chain;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::Data;
  PDGCriteria criteria;
  criteria.nodes.insert(&use);
  SliceQuery slice(pdg_);
  PDGQueryResult result = slice.backward(criteria, local_options, nullptr);
  for (std::unordered_map<Node *, std::set<Node *>>::const_iterator it =
           result.predecessors.begin();
       it != result.predecessors.end(); ++it) {
    for (std::set<Node *>::const_iterator pred_it = it->second.begin();
         pred_it != it->second.end(); ++pred_it) {
      if (*pred_it == nullptr)
        continue;
      chain.push_back(DefUseLink{*pred_it, it->first,
                                 edgeBetween(*pred_it, it->first)});
    }
  }
  return chain;
}

PDGQueryResult DataFlowQuery::liveNodes(const PDGQueryOptions &options) const {
  const NodeSet nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(PDGEdgePreset::Data);
  PDGQueryResult result;
  for (NodeSet::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
    Node *node = *it;
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      if (nodes.count(edge->getDstNode()) != 0) {
        result.nodes.insert(node);
        break;
      }
    }
  }
  result.edges = collectInducedEdges(result.nodes, edge_types);
  return result;
}

PDGQueryResult DataFlowQuery::deadNodes(const PDGQueryOptions &options) const {
  const NodeSet nodes = scopeNodes(pdg_, options.scope);
  PDGQueryResult live = liveNodes(options);
  PDGQueryResult result;
  for (NodeSet::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
    if (live.nodes.count(*it) == 0)
      result.nodes.insert(*it);
  }
  result.edges = collectInducedEdges(result.nodes, edgeTypesForPreset(PDGEdgePreset::Data));
  return result;
}

std::vector<ControllingCondition>
DataFlowQuery::immediateControllers(Node &node) const {
  std::vector<ControllingCondition> controllers;
  const std::set<EdgeType> edge_types = edgeTypesForPreset(PDGEdgePreset::Control);
  for (Node::EdgeSet::const_iterator it = node.getInEdgeSet().begin();
       it != node.getInEdgeSet().end(); ++it) {
    Edge *edge = *it;
    if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
      continue;
    controllers.push_back(ControllingCondition{edge->getSrcNode(),
                                               edge->getEdgeType()});
  }
  return controllers;
}

PDGQueryResult DataFlowQuery::allControllers(const PDGCriteria &criteria,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  PDGQueryOptions control_options = options;
  control_options.edge_preset = PDGEdgePreset::Control;
  SliceQuery slice(pdg_);
  return slice.backward(criteria, control_options, module);
}

PDGQueryResult DataFlowQuery::controlRegion(const PDGCriteria &criteria,
                                            const PDGQueryOptions &options,
                                            const Module *module) const {
  PDGQueryOptions control_options = options;
  control_options.edge_preset = PDGEdgePreset::Control;
  SliceQuery slice(pdg_);
  return slice.forward(criteria, control_options, module);
}

static bool nodesAreInSameFunction(Node &lhs, Node &rhs,
                                   const LLVMQueryContext &llvm_context) {
  if (lhs.getFunc() == nullptr || rhs.getFunc() == nullptr)
    return llvm_context.function == nullptr || lhs.getFunc() == rhs.getFunc();
  return lhs.getFunc() == rhs.getFunc();
}

static bool instructionBlocked(const Instruction &inst,
                               const LLVMQueryContext &llvm_context,
                               std::string &reason) {
  if (isa<PHINode>(&inst)) {
    reason = "PHI nodes are not movable";
    return true;
  }
  if (inst.isTerminator()) {
    reason = "Terminator instructions are not movable";
    return true;
  }
  if (isa<AllocaInst>(&inst)) {
    reason = "Alloca instructions are not movable";
    return true;
  }
  if ((inst.mayReadOrWriteMemory() || inst.mayHaveSideEffects()) &&
      llvm_context.memory_ssa == nullptr) {
    reason = "MemorySSA is required for memory-affecting motion";
    return true;
  }
  if (inst.mayThrow()) {
    reason = "Potentially throwing instructions are not movable";
    return true;
  }
  return false;
}

MotionCheckResult
TransformQuery::canMoveEarlier(Node &moving_node, Node &anchor_node,
                               const LLVMQueryContext &llvm_context,
                               const PDGQueryOptions &options) const {
  MotionCheckResult result;
  result.moving_node = &moving_node;
  result.anchor_node = &anchor_node;

  if (&moving_node == &anchor_node) {
    result.legal = true;
    result.reason = "Trivial move";
    return result;
  }

  if (!nodesAreInSameFunction(moving_node, anchor_node, llvm_context)) {
    result.reason = "Node motion across functions is disallowed";
    return result;
  }

  const Instruction *moving_inst = dyn_cast_or_null<Instruction>(moving_node.getValue());
  const Instruction *anchor_inst = dyn_cast_or_null<Instruction>(anchor_node.getValue());
  if (moving_inst != nullptr &&
      instructionBlocked(*moving_inst, llvm_context, result.reason))
    return result;

  if (llvm_context.dominator_tree != nullptr && moving_inst != nullptr &&
      anchor_inst != nullptr &&
      !llvm_context.dominator_tree->dominates(anchor_inst, moving_inst)) {
    result.reason = "Anchor does not dominate moving instruction";
    return result;
  }

  PDGQueryOptions query_options = options;
  query_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);
  PDGCriteria source_criteria;
  PDGCriteria target_criteria;
  source_criteria.nodes.insert(&anchor_node);
  target_criteria.nodes.insert(&moving_node);
  PDGQueryResult path = query.shortestPath(source_criteria, target_criteria,
                                           query_options, nullptr);
  if (!path.witness_paths.empty()) {
    result.reason = "Anchor transitively constrains moving node";
    result.blocking_path = path.witness_paths.front().nodes;
    result.blocking_edge_types = path.witness_paths.front().edge_types;
    return result;
  }

  result.legal = true;
  result.reason = "No blocking dependence found";
  if (llvm_context.memory_ssa == nullptr)
    result.diagnostics.notes.push_back(
        "Performed conservative motion check without MemorySSA");
  return result;
}

MotionCheckResult
TransformQuery::canMoveLater(Node &moving_node, Node &anchor_node,
                             const LLVMQueryContext &llvm_context,
                             const PDGQueryOptions &options) const {
  MotionCheckResult result;
  result.moving_node = &moving_node;
  result.anchor_node = &anchor_node;

  if (&moving_node == &anchor_node) {
    result.legal = true;
    result.reason = "Trivial move";
    return result;
  }

  if (!nodesAreInSameFunction(moving_node, anchor_node, llvm_context)) {
    result.reason = "Node motion across functions is disallowed";
    return result;
  }

  const Instruction *moving_inst = dyn_cast_or_null<Instruction>(moving_node.getValue());
  const Instruction *anchor_inst = dyn_cast_or_null<Instruction>(anchor_node.getValue());
  if (moving_inst != nullptr &&
      instructionBlocked(*moving_inst, llvm_context, result.reason))
    return result;

  if (llvm_context.post_dominator_tree != nullptr && moving_inst != nullptr &&
      anchor_inst != nullptr &&
      !llvm_context.post_dominator_tree->dominates(anchor_inst->getParent(),
                                                   moving_inst->getParent())) {
    result.reason = "Anchor does not post-dominate moving instruction";
    return result;
  }

  PDGQueryOptions query_options = options;
  query_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);
  PDGCriteria source_criteria;
  PDGCriteria target_criteria;
  source_criteria.nodes.insert(&moving_node);
  target_criteria.nodes.insert(&anchor_node);
  PDGQueryResult path = query.shortestPath(source_criteria, target_criteria,
                                           query_options, nullptr);
  if (!path.witness_paths.empty()) {
    result.reason = "Moving node transitively constrains anchor";
    result.blocking_path = path.witness_paths.front().nodes;
    result.blocking_edge_types = path.witness_paths.front().edge_types;
    return result;
  }

  result.legal = true;
  result.reason = "No blocking dependence found";
  if (llvm_context.memory_ssa == nullptr)
    result.diagnostics.notes.push_back(
        "Performed conservative motion check without MemorySSA");
  return result;
}

IndependenceCheckResult
TransformQuery::independent(Node &a, Node &b,
                            const LLVMQueryContext &llvm_context,
                            const PDGQueryOptions &options) const {
  (void)llvm_context;
  IndependenceCheckResult result;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);

  PDGCriteria a_criteria;
  PDGCriteria b_criteria;
  a_criteria.nodes.insert(&a);
  b_criteria.nodes.insert(&b);

  PDGQueryResult ab = query.shortestPath(a_criteria, b_criteria, local_options,
                                         nullptr);
  PDGQueryResult ba = query.shortestPath(b_criteria, a_criteria, local_options,
                                         nullptr);
  if (!ab.witness_paths.empty()) {
    result.witness_path_ab = ab.witness_paths.front().nodes;
    result.witness_edge_types_ab = ab.witness_paths.front().edge_types;
  }
  if (!ba.witness_paths.empty()) {
    result.witness_path_ba = ba.witness_paths.front().nodes;
    result.witness_edge_types_ba = ba.witness_paths.front().edge_types;
  }
  result.independent =
      result.witness_path_ab.empty() && result.witness_path_ba.empty();
  return result;
}

PDGQueryResult TransformQuery::readySet(const PDGQueryScope &scope,
                                        const NodeSet &scheduled,
                                        const LLVMQueryContext &llvm_context,
                                        const PDGQueryOptions &options) const {
  (void)llvm_context;
  PDGQueryResult result;
  const NodeSet region = scopeNodes(pdg_, scope);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    if (node == nullptr || scheduled.count(node) != 0)
      continue;
    bool ready = true;
    for (Node::EdgeSet::const_iterator edge_it = node->getInEdgeSet().begin();
         edge_it != node->getInEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *pred = edge->getSrcNode();
      if (pred != nullptr && region.count(pred) != 0 &&
          scheduled.count(pred) == 0) {
        ready = false;
        break;
      }
    }
    if (ready)
      result.nodes.insert(node);
  }

  result.edges = collectInducedEdges(result.nodes,
                                     edgeTypesForPreset(options.edge_preset));
  return result;
}

std::vector<NodeSet>
TransformQuery::stronglyConnectedComponents(const PDGQueryScope &scope,
                                            const LLVMQueryContext &llvm_context,
                                            const PDGQueryOptions &options) const {
  (void)llvm_context;
  const NodeSet region = scopeNodes(pdg_, scope);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);
  std::unordered_map<Node *, int> index;
  std::unordered_map<Node *, int> lowlink;
  std::unordered_set<Node *> on_stack;
  std::vector<Node *> stack;
  std::vector<NodeSet> components;
  int next_index = 0;

  std::function<void(Node *)> visit = [&](Node *node) {
    index[node] = next_index;
    lowlink[node] = next_index;
    next_index++;
    stack.push_back(node);
    on_stack.insert(node);

    for (Node::EdgeSet::const_iterator it = node->getOutEdgeSet().begin();
         it != node->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *succ = edge->getDstNode();
      if (succ == nullptr || region.count(succ) == 0)
        continue;
      if (index.count(succ) == 0) {
        visit(succ);
        lowlink[node] = std::min(lowlink[node], lowlink[succ]);
      } else if (on_stack.count(succ) != 0) {
        lowlink[node] = std::min(lowlink[node], index[succ]);
      }
    }

    if (lowlink[node] == index[node]) {
      NodeSet component;
      while (!stack.empty()) {
        Node *member = stack.back();
        stack.pop_back();
        on_stack.erase(member);
        component.insert(member);
        if (member == node)
          break;
      }
      components.push_back(component);
    }
  };

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    if (index.count(*it) == 0)
      visit(*it);
  }
  return components;
}

std::vector<NodeSet>
TransformQuery::topologicalLevels(const PDGQueryScope &scope,
                                  const LLVMQueryContext &llvm_context,
                                  const PDGQueryOptions &options) const {
  (void)options;
  std::vector<NodeSet> levels;
  const NodeSet region = scopeNodes(pdg_, scope);
  if (region.empty())
    return levels;

  std::vector<NodeSet> components =
      stronglyConnectedComponents(scope, llvm_context, options);
  std::unordered_map<Node *, size_t> node_to_component;
  for (size_t index = 0; index < components.size(); ++index) {
    for (NodeSet::const_iterator it = components[index].begin();
         it != components[index].end(); ++it)
      node_to_component[*it] = index;
  }

  std::vector<std::set<size_t>> adjacency(components.size());
  std::vector<size_t> indegree(components.size(), 0);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);
  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *succ = edge->getDstNode();
      if (succ == nullptr || region.count(succ) == 0)
        continue;
      size_t from = node_to_component[node];
      size_t to = node_to_component[succ];
      if (from != to && adjacency[from].insert(to).second)
        indegree[to]++;
    }
  }

  std::queue<size_t> ready;
  for (size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0)
      ready.push(i);
  }

  while (!ready.empty()) {
    size_t layer_size = ready.size();
    NodeSet level;
    for (size_t i = 0; i < layer_size; ++i) {
      size_t component = ready.front();
      ready.pop();
      level.insert(components[component].begin(), components[component].end());
      for (std::set<size_t>::const_iterator it =
               adjacency[component].begin();
           it != adjacency[component].end(); ++it) {
        if (--indegree[*it] == 0)
          ready.push(*it);
      }
    }
    levels.push_back(level);
  }

  return levels;
}

size_t TransformQuery::criticalPathLength(const PDGQueryScope &scope,
                                          const LLVMQueryContext &llvm_context,
                                          const PDGQueryOptions &options) const {
  std::vector<NodeSet> levels = topologicalLevels(scope, llvm_context, options);
  size_t count = 0;
  for (size_t i = 0; i < levels.size(); ++i)
    count += levels[i].empty() ? 0 : 1;
  return count == 0 ? 0 : count - 1;
}

bool DiffQueryResult::isIdentical() const {
  for (size_t i = 0; i < node_diffs.size(); ++i) {
    if (node_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  for (size_t i = 0; i < edge_diffs.size(); ++i) {
    if (edge_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  return true;
}

static bool nodesMatch(Node *lhs, Node *rhs, NodeMatchStrategy strategy) {
  if (strategy == NodeMatchStrategy::PointerIdentity)
    return lhs == rhs;
  return stableNodeKey(lhs) == stableNodeKey(rhs);
}

DiffQueryResult DiffQuery::diff(const PDGQueryResult &before,
                                const PDGQueryResult &after,
                                const PDGQueryOptions &options) const {
  DiffQueryResult result;
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);
  EdgeSet before_edges = collectInducedEdges(before.nodes, edge_types);
  EdgeSet after_edges = collectInducedEdges(after.nodes, edge_types);

  std::unordered_set<Node *> matched_after_nodes;
  for (NodeSet::const_iterator it = before.nodes.begin(); it != before.nodes.end();
       ++it) {
    Node *node = *it;
    bool matched = false;
    for (NodeSet::const_iterator jt = after.nodes.begin(); jt != after.nodes.end();
         ++jt) {
      if (matched_after_nodes.count(*jt) != 0)
        continue;
      if (nodesMatch(node, *jt, strategy_)) {
        matched = true;
        matched_after_nodes.insert(*jt);
        break;
      }
    }
    result.node_diffs.push_back(
        NodeDiffEntry{node, matched ? DiffKind::Preserved : DiffKind::Removed});
  }
  for (NodeSet::const_iterator it = after.nodes.begin(); it != after.nodes.end();
       ++it) {
    if (matched_after_nodes.count(*it) == 0)
      result.node_diffs.push_back(NodeDiffEntry{*it, DiffKind::Added});
  }

  std::unordered_set<Edge *> matched_after_edges;
  for (EdgeSet::const_iterator it = before_edges.begin(); it != before_edges.end();
       ++it) {
    Edge *edge = *it;
    bool matched = false;
    for (EdgeSet::const_iterator jt = after_edges.begin(); jt != after_edges.end();
         ++jt) {
      Edge *candidate = *jt;
      if (matched_after_edges.count(candidate) != 0)
        continue;
      if (edge->getEdgeType() == candidate->getEdgeType() &&
          nodesMatch(edge->getSrcNode(), candidate->getSrcNode(), strategy_) &&
          nodesMatch(edge->getDstNode(), candidate->getDstNode(), strategy_)) {
        matched = true;
        matched_after_edges.insert(candidate);
        break;
      }
    }
    result.edge_diffs.push_back(
        EdgeDiffEntry{edge, matched ? DiffKind::Preserved : DiffKind::Removed});
  }
  for (EdgeSet::const_iterator it = after_edges.begin(); it != after_edges.end();
       ++it) {
    if (matched_after_edges.count(*it) == 0)
      result.edge_diffs.push_back(EdgeDiffEntry{*it, DiffKind::Added});
  }

  for (size_t i = 0; i < result.node_diffs.size(); ++i) {
    if (result.node_diffs[i].kind == DiffKind::Preserved)
      continue;
    const std::string function = functionNameForNode(result.node_diffs[i].node);
    if (!function.empty())
      result.impact_summary.functions[function]++;
    const std::string source = sourceKeyForNode(result.node_diffs[i].node);
    if (!source.empty())
      result.impact_summary.source_locations[source]++;
  }

  return result;
}

DiffQueryResult DiffQuery::diff(const PDGQueryScope &before,
                                const PDGQueryScope &after,
                                const PDGQueryOptions &options) const {
  PDGQueryResult before_result;
  before_result.nodes = scopeNodes(pdg_, before);
  before_result.edges =
      collectInducedEdges(before_result.nodes,
                          edgeTypesForPreset(options.edge_preset));
  PDGQueryResult after_result;
  after_result.nodes = scopeNodes(pdg_, after);
  after_result.edges =
      collectInducedEdges(after_result.nodes,
                          edgeTypesForPreset(options.edge_preset));
  return diff(before_result, after_result, options);
}

} // namespace pdg
