/**
 * @file CallWrapper.cpp
 * @brief Implementation of CallWrapper class for call site representation in
 * the PDG
 *
 * This file implements the CallWrapper class, which encapsulates a function
 * call site (CallInst) and provides utilities for PDG construction and analysis
 * at function call boundaries. The wrapper manages call-specific information
 * needed for dependency analysis.
 *
 * Key features:
 * - Management of call site nodes
 * - Tracking actual arguments and their parameter trees
 * - Handling of return values and their corresponding trees
 * - Supporting both direct and indirect function calls
 * - Creating appropriate tree structures for parameter passing analysis
 * - Facilitating inter-procedural data flow analysis
 *
 * CallWrapper objects work closely with FunctionWrapper objects to model how
 * data flows between caller and callee functions in the PDG system.
 */

#include "IR/PDG/Core/CallWrapper.h"

using namespace llvm;

/**
 * @brief Builds actual parameter trees for function arguments at a call site.
 *
 * For each argument passed to the function call, this method constructs a
 * corresponding "ActualIn" and "ActualOut" tree. These trees mirror the
 * structure of the "FormalIn" trees of the callee function, enabling
 * field-sensitive parameter mapping.
 *
 * @param callee_fw The FunctionWrapper of the function being called.
 */
void pdg::CallWrapper::buildActualTreeForArgs(FunctionWrapper &callee_fw) {
  Function *called_func = callee_fw.getFunc();
  // we don't handle varidic function at the moment
  if (called_func->isVarArg())
    return;
  // construct actual tree based on the type signature of callee
  auto formal_arg_list = callee_fw.getArgList();
  assert(_arg_list.size() == formal_arg_list.size() &&
         "actual/formal arg size don't match!");
  // iterate through actual param list and construct actual tree by copying
  // formal tree
  auto actual_arg_iter = _arg_list.begin();
  auto formal_arg_iter = formal_arg_list.begin();

  while (actual_arg_iter != _arg_list.end()) {
    Tree *arg_formal_in_tree = callee_fw.getArgFormalInTree(**formal_arg_iter);
    if (!arg_formal_in_tree) // in some case, not each parameter has tree, for
                             // example, a function with structure parameter
    {
      actual_arg_iter++;
      formal_arg_iter++;
      continue;
    }
    // build actual in tree, copying the formal_in tree structure at the moment
    Tree *arg_actual_in_tree = new Tree(*arg_formal_in_tree);
    arg_actual_in_tree->setBaseVal(**actual_arg_iter);
    arg_actual_in_tree->setTreeNodeType(GraphNodeType::PARAM_ACTUALIN);
    TreeNode *actual_in_root_node = arg_actual_in_tree->getRootNode();
    actual_in_root_node->addAddrVar(**actual_arg_iter);
    _arg_actual_in_tree_map.insert(
        std::make_pair(*actual_arg_iter, arg_actual_in_tree));
    // build actual out tree
    Tree *arg_actual_out_tree = new Tree(*arg_formal_in_tree);
    arg_actual_out_tree->setBaseVal(**actual_arg_iter);
    arg_actual_out_tree->setTreeNodeType(GraphNodeType::PARAM_ACTUALOUT);
    TreeNode *actual_out_root_node = arg_actual_out_tree->getRootNode();
    actual_out_root_node->addAddrVar(**actual_arg_iter);
    _arg_actual_out_tree_map.insert(
        std::make_pair(*actual_arg_iter, arg_actual_out_tree));
    ++actual_arg_iter;
    ++formal_arg_iter;
  }
}

void pdg::CallWrapper::buildActualTreesForRetVal(FunctionWrapper &callee_fw) {
  Tree *ret_formal_in_tree = callee_fw.getRetFormalInTree();
  if (!ret_formal_in_tree)
    return;
  // build actual in tree, copying the formal_in tree structure at the moment
  Tree *ret_actual_in_tree = new Tree(*ret_formal_in_tree);
  ret_actual_in_tree->setTreeNodeType(GraphNodeType::PARAM_ACTUALIN);
  TreeNode *ret_actual_in_root_node = ret_actual_in_tree->getRootNode();
  ret_actual_in_root_node->addAddrVar(*_call_inst);
  _ret_val_actual_in_tree = ret_actual_in_tree;

  // build actual out tree
  Tree *ret_actual_out_tree = new Tree(*ret_formal_in_tree);
  ret_actual_out_tree->setTreeNodeType(GraphNodeType::PARAM_ACTUALOUT);
  TreeNode *ret_actual_out_root_node = ret_actual_out_tree->getRootNode();
  ret_actual_out_root_node->addAddrVar(*_call_inst);
  _ret_val_actual_out_tree = ret_actual_out_tree;
}

pdg::Tree *pdg::CallWrapper::getArgActualInTree(Value &actual_arg) {
  auto iter = _arg_actual_in_tree_map.find(&actual_arg);
  if (iter == _arg_actual_in_tree_map.end())
    return nullptr;
  return _arg_actual_in_tree_map[&actual_arg];
}

/**
 * @brief Retrieves the ActualOut tree for a specific argument.
 *
 * @param actual_arg The argument value.
 * @return A pointer to the Tree structure, or nullptr if not found.
 */
pdg::Tree *pdg::CallWrapper::getArgActualOutTree(Value &actual_arg) {
  auto iter = _arg_actual_out_tree_map.find(&actual_arg);
  if (iter == _arg_actual_out_tree_map.end())
    return nullptr;
  return _arg_actual_out_tree_map[&actual_arg];
}
