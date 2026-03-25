/**
 * @file FunctionWrapper.h
 * @brief Header for FunctionWrapper class
 */

#ifndef FUNCTIONWRAPPER_H_
#define FUNCTIONWRAPPER_H_

#include "IR/PDG/Core/Tree.h"
#include "IR/PDG/Support/LLVMEssentials.h"
#include "IR/PDG/Support/PDGUtils.h"

namespace pdg {
/**
 * @brief Wrapper class for LLVM Function in the PDG
 *
 * This class encapsulates an LLVM Function and provides additional
 * functionality needed for Program Dependency Graph construction and analysis.
 * It maintains function-specific information such as:
 * - Entry node in the PDG
 * - Lists of relevant instructions (allocas, calls, returns, etc.)
 * - Trees representing formal arguments and return values (for field-sensitive
 * analysis)
 * - Class membership information (if the function is a method)
 */
class FunctionWrapper {
public:
  using ArgTreeMap =
      std::map<llvm::Argument *, Tree *, std::less<llvm::Argument *>,
               std::allocator<std::pair<llvm::Argument *const, Tree *>>>;

  /**
   * @brief Constructor
   * @param func The LLVM Function to wrap
   */
  FunctionWrapper(llvm::Function *func) {
    _func = func;
    _ret_val_formal_in_tree = nullptr;
    _ret_val_formal_out_tree = nullptr;
    for (auto *arg_iter = _func->arg_begin(); arg_iter != _func->arg_end();
         arg_iter++) {
      _arg_list.push_back(&*arg_iter);
    }
    _entry_node = new Node(GraphNodeType::FUNC_ENTRY);
    _entry_node->setFunc(*func);
  }

  ~FunctionWrapper() {
    releaseTrees();
  }

  /**
   * @brief Release (null out) all Tree pointers without deleting them.
   *
   * Trees are heap-allocated wrapper objects; their TreeNode contents are owned
   * by the PDG node set and are deleted separately by ProgramGraph::reset().
   * Releasing here frees only the wrapper objects and clears the stored
   * pointers, which avoids leaking Trees across rebuilds.
   */
  void releaseTrees() {
    for (auto &entry : _arg_formal_in_tree_map) {
      delete entry.second;
      entry.second = nullptr;
    }
    for (auto &entry : _arg_formal_out_tree_map) {
      delete entry.second;
      entry.second = nullptr;
    }
    delete _ret_val_formal_in_tree;
    _ret_val_formal_in_tree = nullptr;
    delete _ret_val_formal_out_tree;
    _ret_val_formal_out_tree = nullptr;
  }

  /**
   * @brief Get the underlying LLVM Function
   * @return Pointer to the LLVM Function
   */
  llvm::Function *getFunc() const { return _func; }

  /**
   * @brief Get the PDG entry node for this function
   * @return Pointer to the entry Node
   */
  Node *getEntryNode() { return _entry_node; }

  /**
   * @brief Add an instruction to the appropriate internal list based on its
   * type
   * @param i The instruction to add
   */
  void addInst(llvm::Instruction &i);

  /**
   * @brief Build parameter trees for formal arguments
   *
   * Constructs trees representing the structure of formal arguments, essential
   * for field-sensitive inter-procedural analysis.
   */
  void buildFormalTreeForArgs();

  /**
   * @brief Build parameter trees for the return value
   *
   * Constructs trees representing the structure of the return value.
   */
  void buildFormalTreesForRetVal();

  /**
   * @brief Set the class name if this function is a method
   * @param _cls_name The name of the class
   */
  void setClassName(std::string _cls_name) { _class_name = _cls_name; }

  /**
   * @brief Get the class name if this function is a method
   * @return The class name
   */
  std::string getClassName() { return _class_name; }

  /**
   * @brief Get the debug info type for an argument
   * @param arg The argument
   * @return The DIType associated with the argument
   */
  llvm::DIType *getArgDIType(llvm::Argument &arg);

  /**
   * @brief Get the debug info local variable for an argument
   * @param arg The argument
   * @return The DILocalVariable associated with the argument
   */
  llvm::DILocalVariable *getArgDILocalVar(llvm::Argument &arg);

  /**
   * @brief Get the AllocaInst associated with an argument (if any)
   * @param arg The argument
   * @return The AllocaInst, or nullptr
   */
  llvm::AllocaInst *getArgAllocaInst(llvm::Argument &arg);

  /**
   * @brief Get the formal "in" tree for an argument
   * @param arg The argument
   * @return Pointer to the Tree
   */
  Tree *getArgFormalInTree(llvm::Argument &arg);

  /**
   * @brief Get the formal "out" tree for an argument
   * @param arg The argument
   * @return Pointer to the Tree
   */
  Tree *getArgFormalOutTree(llvm::Argument &arg);

  /**
   * @brief Get the formal "in" tree for the return value
   * @return Pointer to the Tree
   */
  Tree *getRetFormalInTree() { return _ret_val_formal_in_tree; }

  /**
   * @brief Get the formal "out" tree for the return value
   * @return Pointer to the Tree
   */
  Tree *getRetFormalOutTree() { return _ret_val_formal_out_tree; }

  ArgTreeMap &getArgFormalInTreeMap() { return _arg_formal_in_tree_map; }
  ArgTreeMap &getArgFormalOutTreeMap() { return _arg_formal_out_tree_map; }
  std::vector<llvm::AllocaInst *> &getAllocInsts() { return _alloca_insts; }
  std::vector<llvm::DbgDeclareInst *> &getDbgDeclareInsts() {
    return _dbg_declare_insts;
  }
  std::vector<llvm::LoadInst *> &getLoadInsts() { return _load_insts; }
  std::vector<llvm::StoreInst *> &getStoreInsts() { return _store_insts; }
  std::vector<llvm::CallBase *> &getCallInsts() { return _call_insts; }
  std::vector<llvm::ReturnInst *> &getReturnInsts() { return _return_insts; }
  std::vector<llvm::Argument *> &getArgList() { return _arg_list; }

  /**
   * @brief Check if the function has a void return type (represented as null
   * tree)
   * @return True if return value tree is null
   */
  bool hasNullRetVal() { return (_ret_val_formal_in_tree == nullptr); }

private:
  Node *_entry_node;
  std::string _class_name;
  llvm::Function *_func;
  std::vector<llvm::AllocaInst *> _alloca_insts;
  std::vector<llvm::DbgDeclareInst *> _dbg_declare_insts;
  std::vector<llvm::LoadInst *> _load_insts;
  std::vector<llvm::StoreInst *> _store_insts;
  std::vector<llvm::CallBase *> _call_insts;
  std::vector<llvm::ReturnInst *> _return_insts;
  std::vector<llvm::Argument *> _arg_list;
  ArgTreeMap _arg_formal_in_tree_map;
  ArgTreeMap _arg_formal_out_tree_map;
  Tree *_ret_val_formal_in_tree;
  Tree *_ret_val_formal_out_tree;
};
} // namespace pdg

#endif
