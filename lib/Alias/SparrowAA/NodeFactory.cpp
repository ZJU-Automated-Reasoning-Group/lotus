/**
 * @file NodeFactory.cpp
 * @brief Node factory for creating and managing pointer analysis nodes.
 *
 * This file implements the node factory that creates and manages value nodes,
 * object nodes, and special nodes (return, vararg, etc.) for Andersen's pointer
 * analysis. Nodes are context-sensitive and can be merged during optimization.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/NodeFactory.h"

#include "Alias/SparrowAA/Log.h"

#include <limits>
#include <sstream>
#include <unordered_set>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

const unsigned AndersNodeFactory::InvalidIndex =
    std::numeric_limits<unsigned int>::max();

// Limit diagnostic spam for invalid node index queries.
static unsigned InvalidMergeTargetWarnings = 0;

/**
 * @brief Construct the node factory with initial special nodes.
 *
 * Initializes the factory with four special nodes:
 * - Node #0: Universal pointer (unknown pointer)
 * - Node #1: Universal object (unknown object)
 * - Node #2: Null pointer
 * - Node #3: Null object
 *
 * Note: We can't use std::vector::emplace_back() here because AndersNode's
 * constructors are private, hence std::vector cannot see them.
 */
AndersNodeFactory::AndersNodeFactory() {
  // Note that we can't use std::vector::emplace_back() here because
  // AndersNode's constructors are private hence std::vector cannot see it

  // Node #0 is always the universal ptr: the ptr that we don't know anything
  // about.
  nodes.push_back(AndersNode(AndersNode::VALUE_NODE, 0));
  // Node #1 is always the universal obj: the obj that we don't know anything
  // about.
  nodes.push_back(AndersNode(AndersNode::OBJ_NODE, 1));
  // Node #2 always represents the null pointer.
  nodes.push_back(AndersNode(AndersNode::VALUE_NODE, 2));
  // Node #3 is the object that null pointer points to
  nodes.push_back(AndersNode(AndersNode::OBJ_NODE, 3));

  assert(nodes.size() == 4);
}

static constexpr AndersNodeFactory::CtxKey
ctxKeyOrNull(AndersNodeFactory::CtxKey ctx) {
  return ctx;
}

/**
 * @brief Create a value node for an LLVM value in a given context.
 *
 * Value nodes represent pointer values. If a node already exists for this
 * value-context pair, returns the existing node index.
 *
 * @param val The LLVM value (nullptr for temporary nodes)
 * @param ctx The context key
 * @return NodeIndex of the created or existing node
 */
NodeIndex AndersNodeFactory::createValueNode(const Value *val, CtxKey ctx) {
  if (val != nullptr) {
    auto &bucket = valueNodeMap[ctxKeyOrNull(ctx)];
    auto it = bucket.find(val);
    if (it != bucket.end()) {
      // Node already exists, return existing index
      return it->second;
    }
  }

  unsigned nextIdx = nodes.size();
  nodes.push_back(AndersNode(AndersNode::VALUE_NODE, nextIdx, val));
  if (val != nullptr) {
    auto &bucket = valueNodeMap[ctxKeyOrNull(ctx)];
    bucket[val] = nextIdx;
    valueNodeBuckets[val].push_back(nextIdx);
  }

  return nextIdx;
}

/**
 * @brief Create an object node for an LLVM value in a given context.
 *
 * Object nodes represent memory objects that pointers can point to. If a node
 * already exists for this value-context pair, returns the existing node index.
 *
 * @param val The LLVM value (nullptr for temporary objects)
 * @param ctx The context key
 * @return NodeIndex of the created or existing node
 */
NodeIndex AndersNodeFactory::createObjectNode(const Value *val, CtxKey ctx) {
  if (val != nullptr) {
    auto &bucket = objNodeMap[ctxKeyOrNull(ctx)];
    auto it = bucket.find(val);
    if (it != bucket.end()) {
      // Node already exists, return existing index
      return it->second;
    }
  }

  unsigned nextIdx = nodes.size();
  nodes.push_back(AndersNode(AndersNode::OBJ_NODE, nextIdx, val));
  if (val != nullptr) {
    auto &bucket = objNodeMap[ctxKeyOrNull(ctx)];
    bucket[val] = nextIdx;
  }

  return nextIdx;
}

/**
 * @brief Create a return node for a function in a given context.
 *
 * Return nodes represent the return value of a function. Used for functions
 * that return pointers. If a node already exists for this function-context
 * pair, returns the existing node index.
 *
 * @param f The function
 * @param ctx The context key
 * @return NodeIndex of the created or existing return node
 */
NodeIndex AndersNodeFactory::createReturnNode(const llvm::Function *f,
                                              CtxKey ctx) {
  auto &bucket = returnMap[ctxKeyOrNull(ctx)];
  auto it = bucket.find(f);
  if (it != bucket.end()) {
    // Node already exists, return existing index
    return it->second;
  }

  unsigned nextIdx = nodes.size();
  nodes.push_back(AndersNode(AndersNode::VALUE_NODE, nextIdx, f));
  bucket[f] = nextIdx;

  return nextIdx;
}

NodeIndex AndersNodeFactory::createVarargNode(const llvm::Function *f,
                                              CtxKey ctx) {
  auto &bucket = varargMap[ctxKeyOrNull(ctx)];
  auto it = bucket.find(f);
  if (it != bucket.end()) {
    // Node already exists, return existing index
    return it->second;
  }

  unsigned nextIdx = nodes.size();
  nodes.push_back(AndersNode(AndersNode::OBJ_NODE, nextIdx, f));
  bucket[f] = nextIdx;

  return nextIdx;
}

NodeIndex AndersNodeFactory::getValueNodeFor(const Value *val,
                                             CtxKey ctx) const {
  if (const Constant *c = dyn_cast<Constant>(val))
    if (!isa<GlobalValue>(c))
      return getValueNodeForConstant(c, ctx);

  auto ctxIt = valueNodeMap.find(ctxKeyOrNull(ctx));
  if (ctxIt != valueNodeMap.end()) {
    auto itr = ctxIt->second.find(val);
    if (itr != ctxIt->second.end())
      return itr->second;
  }

  // For context-insensitive analysis (ctx == nullptr / single context) there
  // is only one bucket, so the fallback below is safe.  For context-sensitive
  // analysis, crossing context boundaries is unsound for local values; we
  // return InvalidIndex to signal "not found in this context".
  //
  // Exception: global variables and functions are context-independent — they
  // are created once under initialCtx in collectConstraintsForGlobals() and
  // must be accessible from any calling context.  Fall back to the
  // valueNodeBuckets (which aggregates nodes across all contexts) for global
  // values so that a store/load to a global inside a callee context does not
  // trigger a spurious assertion failure.
  if (ctx != nullptr) {
    if (isa<GlobalValue>(val)) {
      // Global: fall back to any context that has a node for this value.
      auto bucket = valueNodeBuckets.find(val);
      if (bucket != valueNodeBuckets.end() && !bucket->second.empty())
        return bucket->second.front();
    }
    // Non-global local value not found in this context.
    return InvalidIndex;
  }

  // Context-insensitive fallback: return the first (and only) node.
  auto bucket = valueNodeBuckets.find(val);
  if (bucket != valueNodeBuckets.end() && !bucket->second.empty())
    return bucket->second.front();
  return InvalidIndex;
}

NodeIndex AndersNodeFactory::getValueNodeForConstant(const llvm::Constant *c,
                                                     CtxKey ctx) const {
  assert(isa<PointerType>(c->getType()) && "Not a constant pointer!");

  if (isa<ConstantPointerNull>(c) || isa<UndefValue>(c))
    return getNullPtrNode();
  else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c))
    return getValueNodeFor(gv, ctx);
  else if (const ConstantExpr *ce = dyn_cast<ConstantExpr>(c)) {
    switch (ce->getOpcode()) {
    // Pointer to any field within a struct is treated as a pointer to the first
    // field
    case Instruction::GetElementPtr:
      return getValueNodeFor(c->getOperand(0), ctx);
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
      return getUniversalPtrNode();
    case Instruction::BitCast:
      return getValueNodeForConstant(ce->getOperand(0), ctx);
    default:
      LOG_ERROR("Constant Expr not yet handled: {}", *ce);
      llvm_unreachable(0);
    }
  }

  llvm_unreachable("Unknown constant pointer!");
  return InvalidIndex;
}

NodeIndex AndersNodeFactory::getObjectNodeFor(const Value *val,
                                              CtxKey ctx) const {
  if (const Constant *c = dyn_cast<Constant>(val))
    if (!isa<GlobalValue>(c))
      return getObjectNodeForConstant(c, ctx);

  auto ctxIt = objNodeMap.find(ctxKeyOrNull(ctx));
  if (ctxIt != objNodeMap.end()) {
    auto itr = ctxIt->second.find(val);
    if (itr != ctxIt->second.end())
      return itr->second;
  }

  // For context-sensitive analysis, do NOT fall back across contexts to avoid
  // returning an object node from an unrelated calling context (Bug 1.4).
  //
  // Exception: global variables are context-independent — their object nodes
  // are created once under initialCtx in collectConstraintsForGlobals() and
  // must be accessible from any calling context.
  if (ctx != nullptr) {
    if (isa<GlobalValue>(val)) {
      // Global: fall back to any context that has an object node for this
      // value.
      for (auto const &entry : objNodeMap) {
        auto found = entry.second.find(val);
        if (found != entry.second.end())
          return found->second;
      }
    }
    return InvalidIndex;
  }

  // Context-insensitive fallback: scan all (single) context.
  for (auto const &entry : objNodeMap) {
    auto found = entry.second.find(val);
    if (found != entry.second.end())
      return found->second;
  }
  return InvalidIndex;
}

NodeIndex AndersNodeFactory::getObjectNodeForConstant(const llvm::Constant *c,
                                                      CtxKey ctx) const {
  assert(isa<PointerType>(c->getType()) && "Not a constant pointer!");

  if (isa<ConstantPointerNull>(c))
    return getNullObjectNode();
  else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c))
    return getObjectNodeFor(gv, ctx);
  else if (const ConstantExpr *ce = dyn_cast<ConstantExpr>(c)) {
    switch (ce->getOpcode()) {
    // Pointer to any field within a struct is treated as a pointer to the first
    // field
    case Instruction::GetElementPtr:
      return getObjectNodeForConstant(ce->getOperand(0), ctx);
    case Instruction::IntToPtr:
    case Instruction::PtrToInt:
      return getUniversalObjNode();
    case Instruction::BitCast:
      return getObjectNodeForConstant(ce->getOperand(0), ctx);
    default:
      LOG_ERROR("Constant Expr not yet handled: {}", *ce);
      llvm_unreachable(0);
    }
  }

  llvm_unreachable("Unknown constant pointer!");
  return InvalidIndex;
}

NodeIndex AndersNodeFactory::getReturnNodeFor(const llvm::Function *f,
                                              CtxKey ctx) const {
  auto ctxIt = returnMap.find(ctxKeyOrNull(ctx));
  if (ctxIt != returnMap.end()) {
    auto itr = ctxIt->second.find(f);
    if (itr != ctxIt->second.end())
      return itr->second;
  }

  // For context-sensitive analysis, do NOT fall back across contexts (Bug 1.4).
  if (ctx != nullptr)
    return InvalidIndex;

  // Context-insensitive fallback.
  for (auto const &entry : returnMap) {
    auto found = entry.second.find(f);
    if (found != entry.second.end())
      return found->second;
  }
  return InvalidIndex;
}

NodeIndex AndersNodeFactory::getVarargNodeFor(const llvm::Function *f,
                                              CtxKey ctx) const {
  auto ctxIt = varargMap.find(ctxKeyOrNull(ctx));
  if (ctxIt != varargMap.end()) {
    auto itr = ctxIt->second.find(f);
    if (itr != ctxIt->second.end())
      return itr->second;
  }

  // For context-sensitive analysis, do NOT fall back across contexts (Bug 1.4).
  if (ctx != nullptr)
    return InvalidIndex;

  // Context-insensitive fallback.
  for (auto const &entry : varargMap) {
    auto found = entry.second.find(f);
    if (found != entry.second.end())
      return found->second;
  }
  return InvalidIndex;
}

void AndersNodeFactory::getValueNodesFor(const llvm::Value *val,
                                         std::vector<NodeIndex> &out) const {
  out.clear();
  auto it = valueNodeBuckets.find(val);
  if (it == valueNodeBuckets.end())
    return;
  out.insert(out.end(), it->second.begin(), it->second.end());
}

void AndersNodeFactory::mergeNode(NodeIndex n0, NodeIndex n1) {
  assert(n0 < nodes.size() && n1 < nodes.size());
  nodes[n1].mergeTarget = n0;
}

NodeIndex AndersNodeFactory::getMergeTarget(NodeIndex n) {
  // Be robust against invalid indices that may be introduced by unexpected
  // constraints. In optimized builds assertions are disabled, so explicitly
  // guard here to avoid out-of-bounds access and hard crashes.
  if (LLVM_UNLIKELY(n >= nodes.size())) {
    if (InvalidMergeTargetWarnings < 10) {
      LOG_WARN("Andersen: getMergeTarget called with invalid node index {} "
               "(numNodes = {})",
               n, nodes.size());
      if (InvalidMergeTargetWarnings == 9)
        LOG_WARN("Andersen: further invalid node index warnings suppressed");
      ++InvalidMergeTargetWarnings;
    }
    // Map any invalid index to the universal pointer node to keep the analysis
    // sound but conservative instead of crashing.
    return getUniversalPtrNode();
  }
  NodeIndex ret = nodes[n].mergeTarget;
  if (ret != n) {
    std::vector<NodeIndex> path(1, n);
    while (ret != nodes[ret].mergeTarget) {
      path.push_back(ret);
      ret = nodes[ret].mergeTarget;
    }
    for (auto idx : path)
      nodes[idx].mergeTarget = ret;
  }
  assert(ret < nodes.size());
  return ret;
}

NodeIndex AndersNodeFactory::getMergeTarget(NodeIndex n) const {
  if (LLVM_UNLIKELY(n >= nodes.size())) {
    if (InvalidMergeTargetWarnings < 10) {
      LOG_WARN("Andersen: getMergeTarget (const) called with invalid node "
               "index {} (numNodes = {})",
               n, nodes.size());
      if (InvalidMergeTargetWarnings == 9)
        LOG_WARN("Andersen: further invalid node index warnings suppressed");
      ++InvalidMergeTargetWarnings;
    }
    return getUniversalPtrNode();
  }
  NodeIndex ret = nodes[n].mergeTarget;
  while (ret != nodes[ret].mergeTarget)
    ret = nodes[ret].mergeTarget;
  return ret;
}

void AndersNodeFactory::getAllocSites(
    std::vector<const llvm::Value *> &allocSites) const {
  allocSites.clear();
  std::unordered_set<const llvm::Value *> uniqueVals;
  for (auto const &ctxEntry : objNodeMap) {
    for (auto const &mapping : ctxEntry.second) {
      if (uniqueVals.insert(mapping.first).second) {
        allocSites.push_back(mapping.first);
      }
    }
  }
}

void AndersNodeFactory::dumpNode(NodeIndex idx) const {
  const AndersNode &n = nodes.at(idx);
  if (n.type == AndersNode::VALUE_NODE)
    errs() << "[V ";
  else if (n.type == AndersNode::OBJ_NODE)
    errs() << "[O ";
  else
    assert(false && "Wrong type number!");
  errs() << "#" << n.idx << "]";
}

void AndersNodeFactory::dumpNodeInfo() const {
  LOG_DEBUG("\n----- Print AndersNodeFactory Info -----");
  for (auto const &node : nodes) {
    std::stringstream ss;
    const AndersNode &n = nodes.at(node.getIndex());
    if (n.type == AndersNode::VALUE_NODE)
      ss << "[V #" << n.idx << "]";
    else if (n.type == AndersNode::OBJ_NODE)
      ss << "[O #" << n.idx << "]";
    else
      assert(false && "Wrong type number!");
    ss << ", val = ";
    const Value *val = node.getValue();
    if (val == nullptr)
      ss << "nullptr";
    else if (isa<Function>(val))
      ss << "  <func> " << val->getName().str();
    else {
      std::string valStr;
      raw_string_ostream rso(valStr);
      rso << *val;
      rso.flush();
      ss << valStr;
    }
    LOG_DEBUG("{}", ss.str());
  }

  LOG_DEBUG("\nReturn Map:");
  for (auto const &ctxEntry : returnMap) {
    for (auto const &mapping : ctxEntry.second)
      LOG_DEBUG("{}  -->>  [Node #{}]", mapping.first->getName().str(),
                mapping.second);
  }
  LOG_DEBUG("\nVararg Map:");
  for (auto const &ctxEntry : varargMap) {
    for (auto const &mapping : ctxEntry.second)
      LOG_DEBUG("{}  -->>  [Node #{}]", mapping.first->getName().str(),
                mapping.second);
  }
  LOG_DEBUG("----- End of Print -----");
}

void AndersNodeFactory::dumpRepInfo() const {
  LOG_DEBUG("\n----- Print Node Merge Info -----");
  for (NodeIndex i = 0, e = nodes.size(); i < e; ++i) {
    NodeIndex rep = getMergeTarget(i);
    if (rep != i)
      LOG_DEBUG("{} -> {}", i, rep);
  }
  LOG_DEBUG("----- End of Print -----");
}
