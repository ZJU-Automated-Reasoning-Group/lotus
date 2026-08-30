#include "IR/PDG/Analysis/Internal/QuerySupport.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

using namespace llvm;

namespace pdg::query_detail {

struct TraversalState {
  Node *node = nullptr;
  size_t depth = 0;
  std::vector<Node *> call_stack;
};

struct StateHash {
  size_t operator()(const std::pair<Node *, std::vector<Node *>> &value) const {
    size_t seed = std::hash<Node *>()(value.first);
    for (Node *node : value.second) {
      seed ^=
          std::hash<Node *>()(node) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

std::string toLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

static bool isControlEdge(EdgeType type) {
  return type == EdgeType::CONTROLDEP_CALLINV ||
         type == EdgeType::CONTROLDEP_CALLRET ||
         type == EdgeType::CONTROLDEP_ENTRY ||
         type == EdgeType::CONTROLDEP_BR || type == EdgeType::CONTROLDEP_IND_BR;
}

bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed) {
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

static bool applyCallStackTransition(bool forward, Node *current,
                                     Node *neighbor, EdgeType edge_type,
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

std::string pointerKey(const void *value) {
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

bool sourceLocationMatches(const PDGSourceLocation &wanted,
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

NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope) {
  NodeSet nodes;
  if (scope.kind == PDGQueryScope::Kind::WholeGraph)
    return allGraphNodes(pdg);

  if (scope.kind == PDGQueryScope::Kind::NodeSet)
    return scope.nodes;

  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    return scope.query_result->nodes;

  if (scope.kind == PDGQueryScope::Kind::Function && scope.function) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end();
         ++it) {
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

EdgeSet collectInducedEdges(const NodeSet &nodes,
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

std::string criteriaCacheKey(const PDGCriteria &criteria,
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

std::string optionsCacheKey(const PDGQueryOptions &options) {
  std::ostringstream os;
  os << static_cast<int>(options.edge_preset) << "|"
     << static_cast<int>(options.context_mode) << "|"
     << static_cast<int>(options.cache_policy) << "|"
     << static_cast<int>(options.slice_flavor) << "|" << options.explain << "|"
     << scopeCacheKey(options.scope) << "|" << options.limits.max_depth << "|"
     << options.limits.max_states << "|" << options.limits.max_paths << "|"
     << options.limits.max_path_length << "|" << options.limits.max_stack_depth;
  return os.str();
}

std::string pathKey(const PDGWitnessPath &path) {
  std::ostringstream os;
  os << static_cast<int>(path.kind) << "|";
  for (Node *node : path.nodes)
    os << pointerKey(node) << ",";
  return os.str();
}

std::string functionNameForNode(Node *node) {
  if (node == nullptr)
    return "";
  if (node->getFunc() != nullptr)
    return node->getFunc()->getName().str();
  const Function *function = dyn_cast_or_null<Function>(node->getValue());
  return function ? function->getName().str() : "";
}

std::string sourceKeyForNode(Node *node) {
  if (node == nullptr)
    return "";
  const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
  if (inst == nullptr || !inst->getDebugLoc())
    return "";
  return locationKey(inst->getDebugLoc());
}

std::string stringifyValue(const Value *value) {
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
  const std::string callee =
      toLower(call->getCalledFunction()->getName().str());
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

NodeSet resolvePropertyCriteria(ProgramGraph &pdg, const Module &module,
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
        moduleHasAnyCall(module,
                         std::vector<std::string>{
                             "__INSTR_mark_pointer", "__INSTR_mark_free",
                             "__INSTR_mark_allocation", "__INSTR_mark_exit"});

    const bool has_overflow_markers =
        (rule.kind == PropertyKind::NoOverflow ||
         rule.kind == PropertyKind::DefBehavior) &&
        moduleHasAnyCall(
            module, std::vector<std::string>{"__VERIFIER_error",
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
              if (isCallToAny(
                      inst, std::vector<std::string>{"__INSTR_mark_pointer",
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
            if (isCallToAny(inst,
                            std::vector<std::string>{
                                "__INSTR_fail", "__assert_fail",
                                "__VERIFIER_silent_exit", "__VERIFIER_exit",
                                "__INSTR_check_assume"}))
              addInstructionNode(inst);
            break;
          case PropertyKind::NullDeref:
            if (has_null_markers) {
              if (isCallToAny(
                      inst, std::vector<std::string>{"__INSTR_mark_pointer"})) {
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

TraversalOutcome traverseGraph(ProgramGraph &pdg, const NodeSet &start_nodes,
                               const std::set<EdgeType> &edge_types,
                               const PDGQueryOptions &options, bool forward,
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

    if (options.limits.max_depth > 0 &&
        state.depth >= options.limits.max_depth) {
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
    const std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>>
        &predecessor_edges,
    const NodeSet &start_nodes, std::vector<EdgeType> *edge_types) {
  std::vector<Node *> path;
  std::vector<EdgeType> local_edge_types;
  Node *current = node;
  std::unordered_set<Node *> seen;

  while (current != nullptr && seen.insert(current).second) {
    path.push_back(current);
    if (start_nodes.count(current) != 0)
      break;
    std::unordered_map<
        Node *, std::vector<std::pair<Node *, EdgeType>>>::const_iterator it =
        predecessor_edges.find(current);
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

  for (NodeSet::const_iterator it = result.nodes.begin();
       it != result.nodes.end(); ++it) {
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

PDGQueryResult resultFromTraversal(const NodeSet &criteria_nodes,
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

void syncCacheEpoch(
    ProgramGraph &pdg, unsigned long long &cache_epoch,
    std::unordered_map<std::string, PDGQueryResult> &result_cache,
    std::unordered_map<std::string, NodeSet> *criteria_cache,
    std::unordered_map<std::string,
                       std::unordered_map<Node *, std::set<Node *>>>
        *closure_cache) {
  if (cache_epoch == pdg.getEpoch())
    return;
  cache_epoch = pdg.getEpoch();
  result_cache.clear();
  if (criteria_cache != nullptr)
    criteria_cache->clear();
  if (closure_cache != nullptr)
    closure_cache->clear();
}

EdgeType edgeBetween(Node *from, Node *to) {
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

} // namespace pdg::query_detail
