/// @file ProgramDependencyGraph.h
/// @brief LLVM ModulePass for building Program Dependency Graph (PDG)
///
/// This file defines the ProgramDependencyGraph pass, the main entry point for
/// constructing PDG from an LLVM module. It orchestrates the construction of
/// intraprocedural and interprocedural dependencies, including data, control,
/// and parameter dependencies between program elements.

#pragma once

#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGCallGraph.h"
#include "IR/PDG/Support/LLVMEssentials.h"

namespace pdg {
/// @brief LLVM ModulePass for building the Program Dependency Graph
///
/// This pass analyzes an LLVM module and constructs a comprehensive PDG that
/// captures all dependencies between program elements:
/// - Data dependencies (def-use chains, memory dependencies)
/// - Control dependencies (execution order, branch conditions)
/// - Parameter dependencies (actual-formal parameter relationships)
/// - Call dependencies (caller-callee relationships)
///
/// The constructed PDG is stored in the singleton ProgramGraph and can be
/// accessed via getPDG() for further analysis and queries.
class ProgramDependencyGraph : public llvm::ModulePass {
public:
  /// @brief Pass identification constant
  static char ID;

  /// @brief Default constructor
  ProgramDependencyGraph() : llvm::ModulePass(ID) {};

  /// @brief Runs the pass on a module
  /// @param M The LLVM module to analyze
  /// @return True if the pass modified the module
  bool runOnModule(llvm::Module &M) override;

  /// @brief Specifies analysis dependencies
  /// @param AU Analysis usage information
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  /// @brief Gets the constructed PDG
  /// @return Pointer to the ProgramGraph singleton
  ProgramGraph *getPDG() { return _PDG; }

  /// @brief Gets the pass name
  /// @return String identifier for this pass
  llvm::StringRef getPassName() const override {
    return "Program Dependency Graph";
  }

  /// @brief Gets the function wrapper for a function
  /// @param F The function to look up
  /// @return Pointer to the function wrapper
  FunctionWrapper *getFuncWrapper(llvm::Function &F) {
    return _PDG->getFuncWrapper(F);
  }

  /// @brief Gets the call wrapper for a call instruction
  /// @param call_inst The call instruction to look up
  /// @return Pointer to the call wrapper
  CallWrapper *getCallWrapper(llvm::CallBase &call_inst) {
    return _PDG->getCallWrapper(call_inst);
  }

  /// @brief Connects global variables with their uses
  void connectGlobalWithUses();

  /// @brief Connects in-tree nodes between source and destination trees
  /// @param src_tree Source tree
  /// @param dst_tree Destination tree
  /// @param edge_type Type of edge to create
  void connectInTrees(Tree *src_tree, Tree *dst_tree, EdgeType edge_type);

  /// @brief Connects out-tree nodes between source and destination trees
  /// @param src_tree Source tree
  /// @param dst_tree Destination tree
  /// @param edge_type Type of edge to create
  void connectOutTrees(Tree *src_tree, Tree *dst_tree, EdgeType edge_type);

  /// @brief Connects caller and callee through a call site
  /// @param cw Call wrapper for the call site
  /// @param fw Function wrapper for the callee
  void connectCallerAndCallee(
      CallWrapper &cw, FunctionWrapper &fw,
      EdgeType call_edge_type = EdgeType::CONTROLDEP_CALLINV);

  /// @brief Builds intraprocedural dependencies for a function
  /// @param F The function to analyze
  void connectIntraprocDependencies(llvm::Function &F);

  /// @brief Builds interprocedural dependencies for a function
  /// @param F The function to analyze
  void connectInterprocDependencies(llvm::Function &F);

  /// @brief Connects class nodes with class methods
  /// @param F The function to process
  void connectClassNodeWithClassMethods(llvm::Function &F);

  /// @brief Connects formal in-tree nodes with address-taken variables
  /// @param formal_in_tree The formal input tree to connect
  void connectFormalInTreeWithAddrVars(Tree &formal_in_tree);

  /// @brief Connects formal out-tree nodes with address-taken variables
  /// @param formal_out_tree The formal output tree to connect
  void connectFormalOutTreeWithAddrVars(Tree &formal_out_tree);

  /// @brief Connects actual in-tree nodes with address-taken variables
  /// @param actual_in_tree The actual input tree to connect
  /// @param ci The call instruction
  void connectActualInTreeWithAddrVars(Tree &actual_in_tree,
                                       llvm::CallBase &ci);

  /// @brief Connects actual out-tree nodes with address-taken variables
  /// @param actual_out_tree The actual output tree to connect
  /// @param ci The call instruction
  void connectActualOutTreeWithAddrVars(Tree &actual_out_tree,
                                        llvm::CallBase &ci);

  /// @brief Checks if dst is reachable from src
  /// @param src Source node
  /// @param dst Destination node
  /// @return True if dst is reachable from src
  bool canReach(Node &src, Node &dst);

  /// @brief Checks if dst is reachable from src, excluding certain edge types
  /// @param src Source node
  /// @param dst Destination node
  /// @param exclude_edge_types Edge types to exclude
  /// @return True if dst is reachable using allowed edges
  bool canReach(Node &src, Node &dst, std::set<EdgeType> exclude_edge_types);

private:
  llvm::Module *_module;
  ProgramGraph *_PDG;
};
} // namespace pdg
