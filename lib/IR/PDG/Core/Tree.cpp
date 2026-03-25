/**
 * @file Tree.cpp
 * @brief Implementation of tree structures for field-sensitive analysis in the
 * PDG
 *
 * This file implements tree data structures that enable field-sensitive
 * analysis of complex data types like structs and arrays in the PDG system.
 * Trees represent the hierarchical structure of data, with each node
 * corresponding to a field or element.
 *
 * Key features:
 * - Field-sensitive representation of struct and array types
 * - Support for parameter passing in function calls
 * - Tracking of "in" trees (for parameters before function calls) and
 *   "out" trees (for potentially modified values after function calls)
 * - Management of tree node access types (read/write)
 * - Tree traversal and manipulation operations
 *
 * Trees are particularly important for inter-procedural analysis, where they
 * model how data flows through function parameters and return values.
 */

#include "IR/PDG/Core/Tree.h"

using namespace llvm;

pdg::TreeNode::TreeNode(const TreeNode &tree_node)
    : Node(tree_node.getNodeType()),
      _tree(nullptr),        // Will be set by tree during tree copy
      _parent_node(nullptr), // Will be set by parent during tree copy
      _depth(tree_node._depth), _di_local_var(tree_node._di_local_var),
      _addr_vars(tree_node._addr_vars), _acc_tag_set(tree_node._acc_tag_set) {
  _func = tree_node.getFunc();
  _node_di_type = tree_node.getDIType();
  _node_type = tree_node.getNodeType();
  // Children will be copied recursively by Tree copy constructor
}

pdg::TreeNode::TreeNode(DIType *di_type, int depth, TreeNode *parent_node,
                        Tree *tree, GraphNodeType node_type)
    : Node(node_type) {
  _node_di_type = di_type;
  _depth = depth;
  _parent_node = parent_node;
  _tree = tree;
}

pdg::TreeNode::TreeNode(Function &f, DIType *di_type, int depth,
                        TreeNode *parent_node, Tree *tree,
                        GraphNodeType node_type)
    : Node(node_type) {
  _node_di_type = di_type;
  _depth = depth;
  _parent_node = parent_node;
  _tree = tree;
  _func = &f;
}

/**
 * @brief Expands the current tree node based on its debug information type.
 *
 * If the node represents an aggregate type (struct/class) or a
 * pointer/reference, this method creates child nodes for fields or pointed-to
 * objects.
 *
 * @return The number of child nodes created.
 */
int pdg::TreeNode::expandNode() {
  // expand debugging information here
  if (_node_di_type == nullptr)
    return 0;
  if (_func == nullptr) {
    // Cannot expand node without function context
    return 0;
  }
  DIType *dt = dbgutils::stripAttributes(*_node_di_type);
  dt = dbgutils::stripMemberTag(*dt);
  // iterate through all the child nodes, build a tree node for each of them.
  if (!dbgutils::isReferenceType(*dt) && !dbgutils::isPointerType(*dt) &&
      !dbgutils::isStructType(*dt) && !dbgutils::isClassType(*dt))
    return 0;

  // expand the referenced object type
  if (dbgutils::isPointerType(*dt) || dbgutils::isReferenceType(*dt)) {
    DIType *pointed_obj_dt = dbgutils::getLowestDIType(*dt);
    if (pointed_obj_dt == nullptr)
      return 0;
    TreeNode *new_child_node = new TreeNode(*_func, pointed_obj_dt, _depth + 1,
                                            this, _tree, getNodeType());
    new_child_node->computeDerivedAddrVarsFromParent();
    _children.push_back(new_child_node);
    this->addNeighbor(*new_child_node, EdgeType::PARAMETER_FIELD);
    return 1;
  }
  // TODO: should change to aggregate type later
  if (dbgutils::isStructType(*dt) || dbgutils::isClassType(*dt)) {
    auto *composite_type = dyn_cast<DICompositeType>(dt);
    if (composite_type == nullptr)
      return 0;
    auto di_node_arr = composite_type->getElements();
    for (unsigned i = 0; i < di_node_arr.size(); i++) {
      DIType *field_di_type = dyn_cast<DIType>(di_node_arr[i]);
      if (field_di_type == nullptr)
        continue;
      TreeNode *new_child_node = new TreeNode(*_func, field_di_type, _depth + 1,
                                              this, _tree, getNodeType());
      new_child_node->computeDerivedAddrVarsFromParent();
      _children.push_back(new_child_node);
      this->addNeighbor(*new_child_node, EdgeType::PARAMETER_FIELD);
    }
    return di_node_arr.size();
  }

  return 0;
}

void pdg::TreeNode::computeDerivedAddrVarsFromParent() {
  if (!_parent_node)
    return;
  if (!_node_di_type)
    return;
  std::unordered_set<llvm::Value *> base_node_addr_vars;
  // handle struct pointer
  auto *grand_parent_node = _parent_node->getParentNode();
  // TODO: now hanlde struct specifically, but should also verify on other
  // aggregate pointer types
  if (grand_parent_node != nullptr &&
      dbgutils::isStructType(*_parent_node->getDIType()) &&
      dbgutils::isStructPointerType(*grand_parent_node->getDIType())) {
    base_node_addr_vars = grand_parent_node->getAddrVars();
  } else
    base_node_addr_vars = _parent_node->getAddrVars();

  bool is_struct_field = false;
  if (dbgutils::isStructType(*_parent_node->getDIType()) ||
      dbgutils::isClassType(*_parent_node->getDIType()))
    is_struct_field = true;

  for (auto *base_node_addr_var : base_node_addr_vars) {
    for (auto *user : base_node_addr_var->users()) {
      // handle load instruction, field should not get the load inst from the
      // sturct pointer.
      if (LoadInst *li = dyn_cast<LoadInst>(user)) {
        if (!is_struct_field)
          _addr_vars.insert(li);
      }
      // handle gep instruction
      if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(user)) {
        if (pdgutils::isGEPOffsetMatchDIOffset(*_node_di_type, *gep))
          _addr_vars.insert(gep);
      }
    }
  }
}

//  ====== Tree =======
pdg::Tree::Tree(const Tree &src_tree)
    : _base_val(src_tree._base_val), _root_node(nullptr),
      _size(src_tree._size) {
  TreeNode *src_tree_root_node = src_tree.getRootNode();
  if (src_tree_root_node == nullptr) {
    return;
  }
  TreeNode *new_root_node = new TreeNode(*src_tree_root_node);
  new_root_node->setParentTreeNode(nullptr);
  new_root_node->setTree(this);
  _root_node = new_root_node;

  // BFS copy of the entire tree.
  // Fix: the TreeNode copy constructor copies _addr_vars from the source node,
  // but child nodes computed via computeDerivedAddrVarsFromParent() store their
  // addr_vars in the source tree's child TreeNode objects.  The copy
  // constructor of TreeNode (which calls Node(node_type)) does copy _addr_vars
  // via the member initializer list, so we just need to make sure we also copy
  // the _addr_vars field explicitly here to be safe, since the TreeNode copy
  // ctor initializes _addr_vars from tree_node._addr_vars.
  std::queue<std::pair<TreeNode *, TreeNode *>> node_queue;
  node_queue.push(std::make_pair(src_tree_root_node, new_root_node));

  while (!node_queue.empty()) {
    TreeNode *src_node = node_queue.front().first;
    TreeNode *dst_node = node_queue.front().second;
    node_queue.pop();

    for (TreeNode *src_child : src_node->getChildNodes()) {
      TreeNode *new_child = new TreeNode(*src_child);
      new_child->setParentTreeNode(dst_node);
      new_child->setTree(this);
      // Explicitly copy addr_vars from the source child to the new child.
      // This is the key fix: computeDerivedAddrVarsFromParent() populates
      // _addr_vars on the source tree's child nodes, but the TreeNode copy
      // constructor's member initializer copies _addr_vars correctly only if
      // the source node's _addr_vars were populated before the copy.
      // We re-copy here to be explicit and future-proof.
      for (auto *addr_var : src_child->getAddrVars()) {
        new_child->addAddrVar(*addr_var);
      }
      dst_node->insertChildNode(new_child);
      dst_node->addNeighbor(*new_child, EdgeType::PARAMETER_FIELD);
      node_queue.push(std::make_pair(src_child, new_child));
    }
  }
}

bool pdg::Tree::isShapeCompatible(const Tree &other) const {
  if (_size != other._size)
    return false;
  if (_root_node == nullptr || other._root_node == nullptr)
    return false;
  auto *lhs_type = _root_node->getDIType();
  auto *rhs_type = other._root_node->getDIType();
  if (lhs_type == nullptr || rhs_type == nullptr)
    return true;
  auto *lhs_stripped = dbgutils::stripAttributes(*lhs_type);
  auto *rhs_stripped = dbgutils::stripAttributes(*rhs_type);
  if (lhs_stripped == nullptr || rhs_stripped == nullptr)
    return true;
  return dbgutils::hasSameDIName(*lhs_stripped, *rhs_stripped);
}

void pdg::Tree::print() {
  if (_root_node == nullptr)
    return;
  std::queue<TreeNode *> node_queue;
  node_queue.push(_root_node);
  while (!node_queue.empty()) {
    int queue_size = node_queue.size();
    while (queue_size > 0) {
      TreeNode *current_node = node_queue.front();
      node_queue.pop();
      queue_size--;
      if (current_node == _root_node)
        if (current_node->getDILocalVar() != nullptr)
          errs() << dbgutils::getSourceLevelVariableName(
                        *current_node->getDILocalVar())
                 << ", ";
        else
          errs() << "<root>, ";
      else {
        if (current_node->getDIType() != nullptr)
          errs() << dbgutils::getSourceLevelVariableName(
                        *current_node->getDIType())
                 << "(" << current_node->getAddrVars().size() << ")"
                 << ", ";
      }
      for (auto *child : current_node->getChildNodes()) {
        node_queue.push(child);
      }
    }
    errs() << "\n";
  }
}

/**
 * @brief Builds the tree up to a maximum depth.
 *
 * Performs a BFS traversal to expand nodes and construct the tree structure,
 * creating a hierarchical representation of the data type.
 *
 * @param max_tree_depth The maximum depth to expand the tree.
 */
void pdg::Tree::build(int max_tree_depth) {
  if (_root_node == nullptr)
    return;
  // Trees built via copy constructor already contain a full shape.
  if (_size > 0)
    return;
  int current_tree_depth = 0;
  std::queue<TreeNode *> node_queue;
  node_queue.push(_root_node);
  while (!node_queue.empty()) // have more child to expand
  {
    current_tree_depth++;
    if (current_tree_depth > max_tree_depth)
      break;
    int queue_size = node_queue.size();
    while (queue_size > 0) {
      queue_size--;
      TreeNode *current_node = node_queue.front();
      node_queue.pop();
      _size++;
      if (current_node->expandNode() > 0) {
        for (auto *child_node : current_node->getChildNodes()) {
          node_queue.push(child_node);
        }
      }
    }
  }
}
