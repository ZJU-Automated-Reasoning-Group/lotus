/**
 * @file FunctionWrapper.cpp
 * @brief Implementation of FunctionWrapper class for function representation in
 * the PDG
 *
 * This file implements the FunctionWrapper class, which encapsulates an LLVM
 * Function and provides various utilities for PDG construction and analysis.
 * The wrapper manages function-specific information needed for dependency
 * analysis.
 *
 * Key features:
 * - Management of function entry nodes
 * - Tracking function arguments and their formal parameter trees
 * - Handling of return values and their corresponding trees
 * - Management of call instructions within the function
 * - Support for function-specific control flow and data flow information
 * - Creating appropriate tree structures for parameter passing analysis
 *
 * FunctionWrapper objects are essential for inter-procedural analysis in the
 * PDG system, as they model how data flows into and out of functions.
 */

#include "IR/PDG/Core/FunctionWrapper.h"

#include "IR/PDG/Support/PDGCommandLineOptions.h"

using namespace llvm;

/**
 * @brief Categorizes and stores an instruction within the function wrapper.
 *
 * Analyzes the instruction type (Alloca, Store, Load, Call, etc.) and adds it
 * to the appropriate internal list for quick access during analysis.
 *
 * @param i The instruction to add.
 */
void pdg::FunctionWrapper::addInst(Instruction &i) {
  if (AllocaInst *ai = dyn_cast<AllocaInst>(&i))
    _alloca_insts.push_back(ai);
  if (StoreInst *si = dyn_cast<StoreInst>(&i))
    _store_insts.push_back(si);
  if (LoadInst *li = dyn_cast<LoadInst>(&i))
    _load_insts.push_back(li);
  if (DbgDeclareInst *dbi = dyn_cast<DbgDeclareInst>(&i))
    _dbg_declare_insts.push_back(dbi);
  if (CallBase *cb = dyn_cast<CallBase>(&i)) {
    if (!isa<DbgDeclareInst>(&i))
      _call_insts.push_back(cb);
  }
  if (ReturnInst *reti = dyn_cast<ReturnInst>(&i))
    _return_insts.push_back(reti);
}

/**
 * @brief Retrieves the debug info type (DIType) for a function argument.
 *
 * Searches the DbgDeclareInsts in the function to find the DILocalVariable
 * corresponding to the given argument, and returns its type.
 *
 * @param arg The argument to look up.
 * @return The DIType of the argument, or nullptr if not found.
 */
DIType *pdg::FunctionWrapper::getArgDIType(Argument &arg) {
  for (auto *dbg_declare_inst : _dbg_declare_insts) {
    DILocalVariable *di_local_var = dbg_declare_inst->getVariable();
    if (!di_local_var)
      continue;
    if (di_local_var->getArg() == arg.getArgNo() + 1 &&
        !di_local_var->getName().empty() &&
        di_local_var->getScope()->getSubprogram() == _func->getSubprogram())
      return di_local_var->getType();
  }
  return nullptr;
}

/**
 * @brief Builds formal parameter trees for all function arguments.
 *
 * For each argument, constructs a "FormalIn" tree representing the incoming
 * data structure and a "FormalOut" tree representing the outgoing data
 * structure (for pointer/reference types). It uses debug information to
 * accurately model the type hierarchy.
 */
void pdg::FunctionWrapper::buildFormalTreeForArgs() {
  for (auto *arg : _arg_list) {
    DILocalVariable *di_local_var = getArgDILocalVar(*arg);
    AllocaInst *arg_alloca_inst = getArgAllocaInst(*arg);
    if (di_local_var == nullptr || arg_alloca_inst == nullptr) {
      if (pdg::DEBUG)
        errs() << "empty di local var: " << _func->getName().str()
               << (di_local_var == nullptr) << " - "
               << (arg_alloca_inst == nullptr) << "\n";
      continue;
    }
    Tree *arg_formal_in_tree = new Tree(*arg);
    TreeNode *formal_in_root_node =
        new TreeNode(*_func, di_local_var->getType(), 0, nullptr,
                     arg_formal_in_tree, GraphNodeType::PARAM_FORMALIN);
    formal_in_root_node->setDILocalVariable(*di_local_var);
    auto addr_taken_vars =
        pdgutils::computeAddrTakenVarsFromAlloc(*arg_alloca_inst);
    for (auto *addr_taken_var : addr_taken_vars) {
      formal_in_root_node->addAddrVar(*addr_taken_var);
    }
    arg_formal_in_tree->setRootNode(*formal_in_root_node);
    arg_formal_in_tree->build();
    _arg_formal_in_tree_map.insert(std::make_pair(arg, arg_formal_in_tree));

    // build formal_out tree by copying fromal_in tree
    Tree *formal_out_tree = new Tree(*arg_formal_in_tree);
    formal_out_tree->setBaseVal(*arg);
    TreeNode *formal_out_root_node = formal_out_tree->getRootNode();
    // copy address variables
    for (auto *addr_var : formal_in_root_node->getAddrVars()) {
      formal_out_root_node->addAddrVar(*addr_var);
    }
    formal_out_tree->setTreeNodeType(GraphNodeType::PARAM_FORMALOUT);
    _arg_formal_out_tree_map.insert(std::make_pair(arg, formal_out_tree));
  }
}

/**
 * @brief Builds formal trees for the function's return value.
 *
 * Constructs "FormalIn" and "FormalOut" trees for the return value, enabling
 * field-sensitive analysis of returned data structures.
 *
 * Fix: the original code always allocated a tree even for void-returning
 * functions, making hasNullRetVal() always return false.  It also called
 * addAddrVar(*ret_val) without checking whether ret_val is nullptr (which it
 * is for "ret void").  We now leave both tree pointers null for void functions
 * so that hasNullRetVal() correctly returns true.
 */
void pdg::FunctionWrapper::buildFormalTreesForRetVal() {
  // Void-returning functions have no return value to model.
  if (_func->getReturnType()->isVoidTy()) {
    _ret_val_formal_in_tree = nullptr;
    _ret_val_formal_out_tree = nullptr;
    return;
  }

  DIType *func_ret_di_type = dbgutils::getFuncRetDIType(*_func);
  // If we cannot determine the return type from debug info, leave trees null
  // so that callers skip return-value parameter edges rather than crashing.
  if (func_ret_di_type == nullptr) {
    if (pdg::DEBUG)
      errs() << "pdg: no return DIType for " << _func->getName()
             << " — skipping return value trees\n";
    _ret_val_formal_in_tree = nullptr;
    _ret_val_formal_out_tree = nullptr;
    return;
  }

  Tree *ret_formal_in_tree = new Tree();
  TreeNode *ret_formal_in_tree_root_node =
      new TreeNode(*_func, func_ret_di_type, 0, nullptr, ret_formal_in_tree,
                   GraphNodeType::PARAM_FORMALIN);
  for (auto *ret_inst : _return_insts) {
    auto *ret_val = ret_inst->getReturnValue();
    if (ret_val != nullptr) // guard: "ret void" has a null return value
      ret_formal_in_tree_root_node->addAddrVar(*ret_val);
  }
  ret_formal_in_tree->setRootNode(*ret_formal_in_tree_root_node);
  ret_formal_in_tree->build();
  _ret_val_formal_in_tree = ret_formal_in_tree;

  Tree *ret_formal_out_tree = new Tree(*ret_formal_in_tree);
  TreeNode *ret_formal_out_tree_root_node = ret_formal_out_tree->getRootNode();
  // copy address variables
  for (auto *addr_var : ret_formal_in_tree_root_node->getAddrVars()) {
    ret_formal_out_tree_root_node->addAddrVar(*addr_var);
  }
  ret_formal_out_tree->setTreeNodeType(GraphNodeType::PARAM_FORMALOUT);
  _ret_val_formal_out_tree = ret_formal_out_tree;
}

/**
 * @brief Retrieves the debug info local variable for a function argument.
 *
 * @param arg The argument to look up.
 * @return The DILocalVariable associated with the argument, or nullptr.
 */
DILocalVariable *pdg::FunctionWrapper::getArgDILocalVar(Argument &arg) {
  for (auto *dbg_declare_inst : _dbg_declare_insts) {
    DILocalVariable *di_local_var = dbg_declare_inst->getVariable();
    if (!di_local_var)
      continue;
    // if (di_local_var->getArg() == arg.getArgNo() + 1 &&
    // !di_local_var->getName().empty() &&
    // di_local_var->getScope()->getSubprogram() == _func->getSubprogram())
    if (di_local_var->getArg() == arg.getArgNo() + 1 &&
        !di_local_var->getName().empty() &&
        di_local_var->getScope()->getSubprogram() == _func->getSubprogram())
      return di_local_var;
  }
  return nullptr;
}

AllocaInst *pdg::FunctionWrapper::getArgAllocaInst(Argument &arg) {
  for (auto *dbg_declare_inst : _dbg_declare_insts) {
    DILocalVariable *di_local_var = dbg_declare_inst->getVariable();
    if (!di_local_var)
      continue;
    if (di_local_var->getArg() == arg.getArgNo() + 1 &&
        !di_local_var->getName().empty() &&
        di_local_var->getScope()->getSubprogram() == _func->getSubprogram()) {
      // For LLVM 14.0.0, we need to handle Metadata conversion correctly
      Metadata *MD = dbg_declare_inst->getRawLocation();
      if (auto *VMD = dyn_cast<ValueAsMetadata>(MD)) {
        if (AllocaInst *ai = dyn_cast<AllocaInst>(VMD->getValue()))
          return ai;
      }
    }
  }
  return nullptr;
}

pdg::Tree *pdg::FunctionWrapper::getArgFormalInTree(Argument &arg) {
  auto iter = _arg_formal_in_tree_map.find(&arg);
  if (iter == _arg_formal_in_tree_map.end())
    return nullptr;
  // assert(iter != _arg_formal_in_tree_map.end() && "cannot find formal tree
  // for arg");
  return _arg_formal_in_tree_map[&arg];
}

/**
 * @brief Retrieves the FormalOut tree for a specific argument.
 *
 * @param arg The argument.
 * @return A pointer to the Tree structure, or nullptr if not found.
 */
pdg::Tree *pdg::FunctionWrapper::getArgFormalOutTree(Argument &arg) {
  auto iter = _arg_formal_out_tree_map.find(&arg);
  if (iter == _arg_formal_out_tree_map.end())
    return nullptr;
  return _arg_formal_out_tree_map[&arg];
}
