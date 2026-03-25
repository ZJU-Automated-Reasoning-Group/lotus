#include "IR/PDG/Analysis/ThinSlicing.h"

#include "IR/PDG/Support/PDGUtils.h"

#include <queue>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace pdg {

namespace {
bool usesValueAsPointerOperand(const Value *value, const Instruction *user) {
  if (value == nullptr || user == nullptr)
    return false;

  if (auto *load = dyn_cast<LoadInst>(user))
    return load->getPointerOperand() == value;
  if (auto *store = dyn_cast<StoreInst>(user))
    return store->getPointerOperand() == value;
  if (auto *gep = dyn_cast<GetElementPtrInst>(user))
    return gep->getPointerOperand() == value;

  return false;
}

bool applyThinCallStackTransition(bool forward, Node *current, Node *neighbor,
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
} // namespace

// ==================== ThinSlicing Implementation ====================

ThinSlicing::NodeSet
ThinSlicing::computeBackwardSlice(Node &seed_node,
                                  const ThinSliceConfig &config,
                                  ThinSliceDiagnostics *diagnostics) {
  return computeBackwardSlice({&seed_node}, config, diagnostics);
}

ThinSlicing::NodeSet
ThinSlicing::computeBackwardSlice(const NodeSet &seed_nodes,
                                  const ThinSliceConfig &config,
                                  ThinSliceDiagnostics *diagnostics) {
  if (config.context_sensitive) {
    return traverseBackwardContextSensitive(seed_nodes, config, diagnostics);
  }
  return traverseBackward(seed_nodes, config, diagnostics);
}

ThinSlicing::NodeSet
ThinSlicing::computeForwardSlice(Node &source_node,
                                 const ThinSliceConfig &config,
                                 ThinSliceDiagnostics *diagnostics) {
  return computeForwardSlice({&source_node}, config, diagnostics);
}

ThinSlicing::NodeSet
ThinSlicing::computeForwardSlice(const NodeSet &source_nodes,
                                 const ThinSliceConfig &config,
                                 ThinSliceDiagnostics *diagnostics) {
  if (config.context_sensitive) {
    return traverseForwardContextSensitive(source_nodes, config, diagnostics);
  }
  return traverseForward(source_nodes, config, diagnostics);
}

ThinSlicing::NodeSet
ThinSlicing::traverseBackward(const NodeSet &start_nodes,
                              const ThinSliceConfig &config,
                              ThinSliceDiagnostics *diagnostics) {
  NodeSet slice;
  VisitedSet visited;
  std::queue<Node *> worklist;

  if (diagnostics)
    *diagnostics = ThinSliceDiagnostics{};

  auto allowed_edge_types = getThinSliceEdgeTypes(config);

  for (auto *node : start_nodes) {
    if (node) {
      worklist.push(node);
      slice.insert(node);
    }
  }

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();

    if (visited.count(current))
      continue;
    visited.insert(current);

    if (config.max_states > 0 && visited.size() > config.max_states) {
      if (diagnostics)
        diagnostics->state_limit_hit = true;
      break;
    }

    bool current_is_field_access = isFieldAccess(current);

    for (auto *edge : current->getInEdgeSet()) {
      if (!edge)
        continue;

      EdgeType et = edge->getEdgeType();

      // Skip control dependencies (core thin slicing rule)
      if (isControlDependencyEdge(et)) {
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      }

      // Check if edge type is in allowed set
      if (!allowed_edge_types.empty() && !allowed_edge_types.count(et))
        continue;

      Node *neighbor = edge->getSrcNode();
      if (!neighbor || visited.count(neighbor))
        continue;

      // For field accesses, skip base pointer dependencies
      // This is the key thin slicing optimization
      if (current_is_field_access &&
          !isValueFlowEdge(edge, neighbor, current)) {
        if (diagnostics)
          diagnostics->base_ptr_deps_excluded++;
        continue;
      }

      slice.insert(neighbor);
      worklist.push(neighbor);
    }
  }

  if (diagnostics)
    diagnostics->slice_size = slice.size();

  return slice;
}

ThinSlicing::NodeSet
ThinSlicing::traverseForward(const NodeSet &start_nodes,
                             const ThinSliceConfig &config,
                             ThinSliceDiagnostics *diagnostics) {
  NodeSet slice;
  VisitedSet visited;
  std::queue<Node *> worklist;

  if (diagnostics)
    *diagnostics = ThinSliceDiagnostics{};

  auto allowed_edge_types = getThinSliceEdgeTypes(config);

  for (auto *node : start_nodes) {
    if (node) {
      worklist.push(node);
      slice.insert(node);
    }
  }

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();

    if (visited.count(current))
      continue;
    visited.insert(current);

    if (config.max_states > 0 && visited.size() > config.max_states) {
      if (diagnostics)
        diagnostics->state_limit_hit = true;
      break;
    }

    for (auto *edge : current->getOutEdgeSet()) {
      if (!edge)
        continue;

      EdgeType et = edge->getEdgeType();

      if (isControlDependencyEdge(et)) {
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      }

      if (!allowed_edge_types.empty() && !allowed_edge_types.count(et))
        continue;

      Node *neighbor = edge->getDstNode();
      if (!neighbor || visited.count(neighbor))
        continue;

      // For stores to fields, we track value flow to readers
      // Skip if this is base pointer propagation, not value propagation
      if (isFieldAccess(neighbor) &&
          !isValueFlowEdge(edge, current, neighbor)) {
        if (diagnostics)
          diagnostics->base_ptr_deps_excluded++;
        continue;
      }

      slice.insert(neighbor);
      worklist.push(neighbor);
    }
  }

  if (diagnostics)
    diagnostics->slice_size = slice.size();

  return slice;
}

// Hash for (Node*, call_stack) pairs in context-sensitive traversal
struct ThinNodeStackHash {
  size_t operator()(const std::pair<Node *, std::vector<Node *>> &p) const {
    size_t hash = std::hash<Node *>{}(p.first);
    for (auto *node : p.second) {
      hash ^=
          std::hash<Node *>{}(node) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    }
    return hash;
  }
};

ThinSlicing::NodeSet ThinSlicing::traverseBackwardContextSensitive(
    const NodeSet &start_nodes, const ThinSliceConfig &config,
    ThinSliceDiagnostics *diagnostics) {
  using VisitedCSSet =
      std::unordered_set<std::pair<Node *, std::vector<Node *>>,
                         ThinNodeStackHash>;

  NodeSet slice;
  VisitedCSSet visited;
  std::queue<std::pair<Node *, std::vector<Node *>>> worklist;

  if (diagnostics)
    *diagnostics = ThinSliceDiagnostics{};

  auto allowed_edge_types = getThinSliceEdgeTypes(config);

  for (auto *node : start_nodes) {
    if (node) {
      worklist.push({node, {}});
      slice.insert(node);
    }
  }

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    std::vector<Node *> call_stack = current_pair.second;
    worklist.pop();

    if (diagnostics) {
      diagnostics->max_stack_depth_reached =
          std::max(diagnostics->max_stack_depth_reached, call_stack.size());
    }

    if (config.max_stack_depth > 0 &&
        call_stack.size() > config.max_stack_depth) {
      if (diagnostics)
        diagnostics->stack_depth_limit_hit = true;
      continue;
    }

    auto state = std::make_pair(current, call_stack);
    if (visited.count(state))
      continue;

    if (config.max_states > 0 && visited.size() + 1 > config.max_states) {
      if (diagnostics)
        diagnostics->state_limit_hit = true;
      break;
    }
    visited.insert(state);

    bool current_is_field_access = isFieldAccess(current);

    for (auto *edge : current->getInEdgeSet()) {
      if (!edge)
        continue;

      EdgeType et = edge->getEdgeType();

      // Get neighbor first (needed for both control and data edges)
      Node *neighbor = edge->getSrcNode();
      if (!neighbor)
        continue;

      std::vector<Node *> new_stack = call_stack;

      // Handle control edges for CFL-reachability stack matching
      // These edges are used for context matching but are NOT added to thin
      // slice
      if (et == EdgeType::CONTROLDEP_CALLRET ||
          et == EdgeType::CONTROLDEP_CALLINV) {
        if (!applyThinCallStackTransition(false, current, neighbor, et,
                                          new_stack)) {
          continue;
        }
        // Control edges are NOT value flow edges - don't add to thin slice
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      } else if (isControlDependencyEdge(et)) {
        // Other control dependencies are excluded from thin slicing
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      }

      if (!allowed_edge_types.empty() && !allowed_edge_types.count(et))
        continue;

      if (current_is_field_access &&
          !isValueFlowEdge(edge, neighbor, current)) {
        if (diagnostics)
          diagnostics->base_ptr_deps_excluded++;
        continue;
      }

      auto new_state = std::make_pair(neighbor, new_stack);
      if (!visited.count(new_state)) {
        slice.insert(neighbor);
        worklist.push({neighbor, new_stack});
      }
    }
  }

  if (diagnostics)
    diagnostics->slice_size = slice.size();

  return slice;
}

ThinSlicing::NodeSet ThinSlicing::traverseForwardContextSensitive(
    const NodeSet &start_nodes, const ThinSliceConfig &config,
    ThinSliceDiagnostics *diagnostics) {
  using VisitedCSSet =
      std::unordered_set<std::pair<Node *, std::vector<Node *>>,
                         ThinNodeStackHash>;

  NodeSet slice;
  VisitedCSSet visited;
  std::queue<std::pair<Node *, std::vector<Node *>>> worklist;

  if (diagnostics)
    *diagnostics = ThinSliceDiagnostics{};

  auto allowed_edge_types = getThinSliceEdgeTypes(config);

  for (auto *node : start_nodes) {
    if (node) {
      worklist.push({node, {}});
      slice.insert(node);
    }
  }

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    std::vector<Node *> call_stack = current_pair.second;
    worklist.pop();

    if (diagnostics) {
      diagnostics->max_stack_depth_reached =
          std::max(diagnostics->max_stack_depth_reached, call_stack.size());
    }

    if (config.max_stack_depth > 0 &&
        call_stack.size() > config.max_stack_depth) {
      if (diagnostics)
        diagnostics->stack_depth_limit_hit = true;
      continue;
    }

    auto state = std::make_pair(current, call_stack);
    if (visited.count(state))
      continue;

    if (config.max_states > 0 && visited.size() + 1 > config.max_states) {
      if (diagnostics)
        diagnostics->state_limit_hit = true;
      break;
    }
    visited.insert(state);

    for (auto *edge : current->getOutEdgeSet()) {
      if (!edge)
        continue;

      EdgeType et = edge->getEdgeType();

      // Get neighbor first (needed for both control and data edges)
      Node *neighbor = edge->getDstNode();
      if (!neighbor)
        continue;

      std::vector<Node *> new_stack = call_stack;

      // Handle control edges for CFL-reachability stack matching
      // These edges are used for context matching but are NOT added to thin
      // slice
      if (et == EdgeType::CONTROLDEP_CALLINV ||
          et == EdgeType::CONTROLDEP_CALLRET) {
        if (!applyThinCallStackTransition(true, current, neighbor, et,
                                          new_stack)) {
          continue;
        }
        // Control edges are NOT value flow edges - don't add to thin slice
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      } else if (isControlDependencyEdge(et)) {
        // Other control dependencies are excluded from thin slicing
        if (diagnostics)
          diagnostics->control_deps_excluded++;
        continue;
      }

      if (!allowed_edge_types.empty() && !allowed_edge_types.count(et))
        continue;

      if (config.max_stack_depth > 0 &&
          new_stack.size() > config.max_stack_depth) {
        if (diagnostics)
          diagnostics->stack_depth_limit_hit = true;
        continue;
      }

      if (isFieldAccess(neighbor) &&
          !isValueFlowEdge(edge, current, neighbor)) {
        if (diagnostics)
          diagnostics->base_ptr_deps_excluded++;
        continue;
      }

      auto new_state = std::make_pair(neighbor, new_stack);
      if (!visited.count(new_state)) {
        slice.insert(neighbor);
        worklist.push({neighbor, new_stack});
      }
    }
  }

  if (diagnostics)
    diagnostics->slice_size = slice.size();

  return slice;
}

std::unordered_map<Node *, ThinSlicing::NodeSet>
ThinSlicing::expandForAliasing(const NodeSet &slice,
                               const ThinSliceConfig &config) {
  std::unordered_map<Node *, NodeSet> expansion_slices;

  for (auto *node : slice) {
    if (!node || !isFieldAccess(node))
      continue;

    Node *base_ptr = getBasePointerNode(node);
    if (!base_ptr || slice.count(base_ptr))
      continue;

    if (expansion_slices.find(base_ptr) == expansion_slices.end()) {
      expansion_slices[base_ptr] = computeBackwardSlice(*base_ptr, config);
    }
  }

  return expansion_slices;
}

bool ThinSlicing::isValueFlowEdge(Edge *edge, Node *src_node, Node *dst_node) {
  if (!edge || !src_node || !dst_node)
    return false;

  EdgeType et = edge->getEdgeType();
  switch (et) {
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

  Value *src_value = src_node->getValue();
  Value *dst_value = dst_node->getValue();
  auto *dst_inst = dyn_cast_or_null<Instruction>(dst_value);
  auto *src_inst = dyn_cast_or_null<Instruction>(src_value);

  if (usesValueAsPointerOperand(src_value, dst_inst))
    return false;

  if (auto *gep = dyn_cast_or_null<GetElementPtrInst>(src_value)) {
    if (usesValueAsPointerOperand(gep, dst_inst))
      return false;
  }

  if (auto *store = dyn_cast_or_null<StoreInst>(dst_value)) {
    return store->getValueOperand() == src_value;
  }

  if (auto *load = dyn_cast_or_null<LoadInst>(dst_value)) {
    return load->getPointerOperand() != src_value;
  }

  if (auto *gep = dyn_cast_or_null<GetElementPtrInst>(dst_value)) {
    return gep->getPointerOperand() != src_value;
  }

  if (auto *store = dyn_cast_or_null<StoreInst>(src_value)) {
    if (dst_value != nullptr && store->getPointerOperand() == dst_value)
      return false;
  }

  if (auto *gep = dyn_cast_or_null<GetElementPtrInst>(src_value)) {
    if (dst_inst != nullptr && usesValueAsPointerOperand(gep, dst_inst))
      return false;
  }

  return src_inst != nullptr || dst_inst != nullptr;
}

bool ThinSlicing::isFieldAccess(Node *node) {
  if (!node)
    return false;

  Value *val = node->getValue();
  if (!val)
    return false;

  if (isa<LoadInst>(val) || isa<StoreInst>(val))
    return true;

  if (isa<GetElementPtrInst>(val))
    return true;

  return false;
}

Node *ThinSlicing::getBasePointerNode(Node *field_access_node) {
  if (!field_access_node)
    return nullptr;

  Value *val = field_access_node->getValue();
  if (!val)
    return nullptr;

  Value *ptr_operand = nullptr;

  if (auto *load_inst = dyn_cast<LoadInst>(val)) {
    ptr_operand = load_inst->getPointerOperand();
  } else if (auto *store_inst = dyn_cast<StoreInst>(val)) {
    ptr_operand = store_inst->getPointerOperand();
  } else if (auto *gep = dyn_cast<GetElementPtrInst>(val)) {
    ptr_operand = gep->getPointerOperand();
  }

  if (!ptr_operand)
    return nullptr;

  return _pdg.getNode(*ptr_operand);
}

bool ThinSlicing::isControlDependencyEdge(EdgeType edge_type) {
  return edge_type == EdgeType::CONTROLDEP_CALLINV ||
         edge_type == EdgeType::CONTROLDEP_CALLRET ||
         edge_type == EdgeType::CONTROLDEP_ENTRY ||
         edge_type == EdgeType::CONTROLDEP_BR ||
         edge_type == EdgeType::CONTROLDEP_IND_BR;
}

bool ThinSlicing::isDataDependencyEdge(EdgeType edge_type) {
  return edge_type == EdgeType::DATA_DEF_USE ||
         edge_type == EdgeType::DATA_RAW || edge_type == EdgeType::DATA_READ ||
         edge_type == EdgeType::DATA_ALIAS || edge_type == EdgeType::DATA_RET ||
         edge_type == EdgeType::VAL_DEP;
}

std::set<EdgeType>
ThinSlicing::getThinSliceEdgeTypes(const ThinSliceConfig &config) {
  std::set<EdgeType> edge_types;

  // Core data flow edges (always included)
  edge_types.insert(EdgeType::DATA_DEF_USE);
  edge_types.insert(EdgeType::DATA_RAW);
  edge_types.insert(EdgeType::DATA_ALIAS);
  edge_types.insert(EdgeType::VAL_DEP);

  if (config.include_return_deps) {
    edge_types.insert(EdgeType::DATA_RET);
  }

  if (config.include_parameter_deps) {
    edge_types.insert(EdgeType::PARAMETER_IN);
    edge_types.insert(EdgeType::PARAMETER_OUT);
    edge_types.insert(EdgeType::PARAMETER_FIELD);
  }

  return edge_types;
}

// ==================== ThinSlicingUtils Implementation ====================

std::set<EdgeType> ThinSlicingUtils::getValueFlowEdgeTypes() {
  return {EdgeType::DATA_DEF_USE, EdgeType::DATA_RAW, EdgeType::DATA_ALIAS,
          EdgeType::DATA_RET,     EdgeType::VAL_DEP,  EdgeType::PARAMETER_IN,
          EdgeType::PARAMETER_OUT};
}

std::set<EdgeType> ThinSlicingUtils::getExcludedEdgeTypes() {
  return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

std::unordered_map<std::string, size_t>
ThinSlicingUtils::compareWithTraditionalSlice(
    const NodeSet &thin_slice, const NodeSet &traditional_slice) {
  std::unordered_map<std::string, size_t> comparison;
  comparison["thin_slice_size"] = thin_slice.size();
  comparison["traditional_slice_size"] = traditional_slice.size();

  size_t thin_only = 0, traditional_only = 0, common = 0;
  for (auto *node : thin_slice) {
    if (traditional_slice.count(node)) {
      common++;
    } else {
      thin_only++;
    }
  }
  for (auto *node : traditional_slice) {
    if (!thin_slice.count(node)) {
      traditional_only++;
    }
  }

  comparison["thin_only_nodes"] = thin_only;
  comparison["traditional_only_nodes"] = traditional_only;
  comparison["common_nodes"] = common;

  if (traditional_slice.size() > 0) {
    comparison["reduction_percent"] = static_cast<size_t>(
        (double)traditional_only / traditional_slice.size() * 100.0);
  }

  return comparison;
}

void ThinSlicingUtils::printThinSlice(const NodeSet &slice,
                                      const std::string &slice_name) {
  errs() << "=============== Thin Slice: " << slice_name
         << " ===============\n";
  errs() << "Slice size: " << slice.size() << " nodes\n";

  for (auto *node : slice) {
    if (!node)
      continue;

    std::string str;
    raw_string_ostream OS(str);
    Value *val = node->getValue();

    if (val) {
      if (auto *f = dyn_cast<Function>(val)) {
        OS << f->getName().str();
      } else if (auto *inst = dyn_cast<Instruction>(val)) {
        OS << *inst;
      } else if (auto *gv = dyn_cast<GlobalVariable>(val)) {
        OS << gv->getName().str();
      } else {
        OS << *val;
      }
      errs() << "  " << pdgutils::rtrim(OS.str()) << " ["
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "]\n";
    } else {
      errs() << "  <no value> ["
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "]\n";
    }
  }
  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t>
ThinSlicingUtils::getThinSliceStatistics(const NodeSet &slice) {
  std::unordered_map<std::string, size_t> stats;
  std::unordered_map<GraphNodeType, size_t> node_type_counts;

  stats["total_nodes"] = slice.size();

  size_t load_nodes = 0, store_nodes = 0, gep_nodes = 0;

  for (auto *node : slice) {
    if (!node)
      continue;

    node_type_counts[node->getNodeType()]++;

    if (isLoadNode(node))
      load_nodes++;
    if (isStoreNode(node))
      store_nodes++;
    if (isGEPNode(node))
      gep_nodes++;
  }

  for (const auto &pair : node_type_counts) {
    stats["node_type_" + pdgutils::getNodeTypeStr(pair.first)] = pair.second;
  }

  stats["load_nodes"] = load_nodes;
  stats["store_nodes"] = store_nodes;
  stats["gep_nodes"] = gep_nodes;
  stats["memory_access_nodes"] = load_nodes + store_nodes;

  return stats;
}

ThinSlicingUtils::NodeSet
ThinSlicingUtils::identifyBasePointerNodes(const NodeSet &slice,
                                           GenericGraph &pdg) {
  NodeSet base_ptrs;

  for (auto *node : slice) {
    if (!node)
      continue;

    Value *val = node->getValue();
    if (!val)
      continue;

    Value *ptr_operand = nullptr;

    if (auto *load_inst = dyn_cast<LoadInst>(val)) {
      ptr_operand = load_inst->getPointerOperand();
    } else if (auto *store_inst = dyn_cast<StoreInst>(val)) {
      ptr_operand = store_inst->getPointerOperand();
    }

    if (ptr_operand) {
      if (Node *ptr_node = pdg.getNode(*ptr_operand)) {
        if (!slice.count(ptr_node)) {
          base_ptrs.insert(ptr_node);
        }
      }
    }
  }

  return base_ptrs;
}

bool ThinSlicingUtils::isLoadNode(Node *node) {
  if (!node)
    return false;
  Value *val = node->getValue();
  return isa_and_nonnull<LoadInst>(val);
}

bool ThinSlicingUtils::isStoreNode(Node *node) {
  if (!node)
    return false;
  Value *val = node->getValue();
  return isa_and_nonnull<StoreInst>(val);
}

bool ThinSlicingUtils::isGEPNode(Node *node) {
  if (!node)
    return false;
  Value *val = node->getValue();
  return isa_and_nonnull<GetElementPtrInst>(val);
}

} // namespace pdg
