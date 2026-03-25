/**
 * @file PDGCallGraph.h
 * @brief Header for PDGCallGraph class
 */

#pragma once
#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Support/LLVMEssentials.h"
#include "IR/PDG/Support/PDGUtils.h"

namespace pdg {
/**
 * @brief Call Graph implementation for the PDG system
 *
 * This class represents the call graph of the program, where nodes are
 * functions and edges represent function calls. It supports:
 * - Construction from LLVM Module
 * - Handling of both direct and indirect calls
 * - Reachability analysis between functions
 * - Path computation between functions
 */
class PDGCallGraph : public GenericGraph {
public:
  using PathVecs = std::vector<std::vector<llvm::Function *>>;

  PDGCallGraph() = default;
  PDGCallGraph(const PDGCallGraph &) = delete;
  PDGCallGraph(PDGCallGraph &&) = delete;
  PDGCallGraph &operator=(const PDGCallGraph &) = delete;
  PDGCallGraph &operator=(PDGCallGraph &&) = delete;

  /**
   * @brief Singleton accessor
   * @return Reference to the singleton instance
   */
  static PDGCallGraph &getInstance() {
    static PDGCallGraph g{};
    return g;
  }

  /**
   * @brief Build the call graph for the given module
   * @param M The LLVM Module
   */
  void build(llvm::Module &M) override;

  bool isBuiltForModule(const llvm::Module &M) const {
    return _is_build && _built_module == &M;
  }

  void reset() {
    GenericGraph::reset();
    _built_module = nullptr;
  }

  /**
   * @brief Identify potential targets for an indirect call
   * @param ci The indirect CallInst
   * @param M The module to search in
   * @return Set of candidate functions
   */
  std::set<llvm::Function *> getIndirectCallCandidates(llvm::CallBase &ci,
                                                       llvm::Module &M);

  /**
   * @brief Check if a function signature matches a call instruction
   * @param ci The CallInst
   * @param f The Function to check against
   * @return True if signatures match
   */
  bool isFuncSignatureMatch(llvm::CallBase &ci, llvm::Function &f);

  /**
   * @brief Check if two LLVM types are structurally equal
   * @param t1 First type
   * @param t2 Second type
   * @return True if equal
   */
  bool isTypeEqual(llvm::Type &t1, llvm::Type &t2);

  /**
   * @brief Check if a source node can reach a sink node in the call graph
   * @param src Source node
   * @param sink Sink node
   * @return True if reachable
   */
  bool canReach(Node &src, Node &sink);

  /**
   * @brief Dump the call graph to stderr
   */
  void dump();

  /**
   * @brief Print all paths from source to sink
   * @param src Source node
   * @param sink Sink node
   */
  void printPaths(Node &src, Node &sink);

  /**
   * @brief Compute all paths from source to sink
   * @param src Source node
   * @param sink Sink node
   * @return Vector of paths (each path is a vector of Functions)
   */
  PathVecs computePaths(Node &src, Node &sink); // compute all pathes

  void computePathsHelper(PathVecs &path_vecs, Node &src, Node &sink,
                          std::vector<llvm::Function *> cur_path,
                          std::unordered_set<llvm::Function *> visited_funcs,
                          bool &found_path);

private:
  llvm::Module *_built_module = nullptr;
};
} // namespace pdg
