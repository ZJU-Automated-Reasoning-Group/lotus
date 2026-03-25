/**
 * @file MPIRankAnalysis.h
 * @brief Symbolic MPI Rank Analysis
 *
 * This file provides symbolic execution for MPI rank-dependent control flow.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace MPI {

struct MPIRankPredicate {
  const llvm::Value *communicator = nullptr;
  bool unknown = true;
  bool universal = false;
  int min_rank = 0;
  int max_rank = -1;
  std::set<int> excluded_ranks;

  static MPIRankPredicate makeUnknown(const llvm::Value *comm = nullptr) {
    MPIRankPredicate pred;
    pred.communicator = comm;
    pred.unknown = true;
    pred.universal = false;
    pred.min_rank = 0;
    pred.max_rank = -1;
    return pred;
  }

  static MPIRankPredicate makeUniversal(const llvm::Value *comm = nullptr,
                                        int upper_bound = -1) {
    MPIRankPredicate pred;
    pred.communicator = comm;
    pred.unknown = false;
    pred.universal = true;
    pred.min_rank = 0;
    pred.max_rank = upper_bound;
    return pred;
  }

  static MPIRankPredicate makeConcrete(int rank,
                                       const llvm::Value *comm = nullptr) {
    MPIRankPredicate pred;
    pred.communicator = comm;
    pred.unknown = false;
    pred.universal = false;
    pred.min_rank = rank;
    pred.max_rank = rank;
    return pred;
  }

  static MPIRankPredicate makeRange(int min_rank, int max_rank,
                                    const llvm::Value *comm = nullptr) {
    MPIRankPredicate pred;
    pred.communicator = comm;
    pred.unknown = false;
    pred.universal = false;
    pred.min_rank = min_rank;
    pred.max_rank = max_rank;
    return pred;
  }

  bool isConcrete() const {
    return !unknown && !universal && min_rank == max_rank &&
           excluded_ranks.empty();
  }

  bool isRange() const {
    return !unknown && (!universal || !excluded_ranks.empty() || max_rank >= 0) &&
           !isConcrete();
  }

  bool constrainsParticipants() const {
    if (unknown) {
      return false;
    }
    if (!universal) {
      return true;
    }
    return !excluded_ranks.empty();
  }

  bool contains(int rank) const;
  bool mayOverlap(const MPIRankPredicate &other) const;
  bool mustEqual(const MPIRankPredicate &other) const;
  std::string toKey() const;

  bool operator<(const MPIRankPredicate &other) const {
    if (communicator != other.communicator) {
      return communicator < other.communicator;
    }
    if (unknown != other.unknown) {
      return unknown < other.unknown;
    }
    if (universal != other.universal) {
      return universal < other.universal;
    }
    if (min_rank != other.min_rank) {
      return min_rank < other.min_rank;
    }
    if (max_rank != other.max_rank) {
      return max_rank < other.max_rank;
    }
    return excluded_ranks < other.excluded_ranks;
  }
};

/**
 * @brief Symbolic rank expression
 */
class RankExpr {
public:
  enum Kind {
    Concrete,    ///< Concrete rank value (e.g., 0, 1, 2)
    Symbolic,    ///< Symbolic rank (from MPI_Comm_rank)
    Range,       ///< Range of ranks [min, max]
    Unknown      ///< Unknown rank
  };

  Kind kind;
  int concrete_value;  ///< For Concrete kind
  int range_min;       ///< For Range kind
  int range_max;       ///< For Range kind
  const llvm::Value *communicator = nullptr; ///< Communicator that owns the rank

  RankExpr() : kind(Unknown), concrete_value(-1), range_min(0), range_max(-1) {}

  static RankExpr makeConcrete(int rank, const llvm::Value *comm = nullptr) {
    RankExpr expr;
    expr.kind = Concrete;
    expr.concrete_value = rank;
    expr.communicator = comm;
    return expr;
  }

  static RankExpr makeSymbolic(const llvm::Value *comm = nullptr) {
    RankExpr expr;
    expr.kind = Symbolic;
    expr.communicator = comm;
    return expr;
  }

  static RankExpr makeRange(int min, int max,
                            const llvm::Value *comm = nullptr) {
    RankExpr expr;
    expr.kind = Range;
    expr.range_min = min;
    expr.range_max = max;
    expr.communicator = comm;
    return expr;
  }

  bool mayEqual(const RankExpr &other) const;
  bool mustEqual(const RankExpr &other) const;
};

/**
 * @class MPIRankAnalysis
 * @brief Symbolic analysis of MPI rank values
 *
 * Tracks MPI rank values through the program to enable precise
 * analysis of rank-dependent control flow and communication patterns.
 */
class MPIRankAnalysis {
public:
  enum class ReachabilityKind { AllRanks, SomeRanks, Unknown };

  explicit MPIRankAnalysis(llvm::Module &module);

  /**
   * @brief Run the rank analysis
   */
  void analyze();

  /**
   * @brief Get the rank expression for a value
   */
  RankExpr getRankExpr(const llvm::Value *val) const;

  /**
   * @brief Get the rank predicate for a value
   */
  MPIRankPredicate getRankPredicate(const llvm::Value *val) const;

  /**
   * @brief Get the rank at a specific instruction
   */
  RankExpr getRankAtInstruction(const llvm::Instruction *inst) const;

  MPIRankPredicate
  getRankPredicateAtInstruction(const llvm::Instruction *inst) const;

  ReachabilityKind
  getReachabilityAtInstruction(const llvm::Instruction *inst) const;

  size_t getPredicateClassAtInstruction(const llvm::Instruction *inst) const;
  size_t getParticipantClassAtInstruction(const llvm::Instruction *inst) const;

  /**
   * @brief Check if two instructions are in the same rank
   */
  bool sameRank(const llvm::Instruction *i1, const llvm::Instruction *i2) const;

  /**
   * @brief Get all ranks that may execute an instruction
   */
  std::set<int> getPossibleRanks(const llvm::Instruction *inst) const;

  static constexpr int defaultCommSizeUpperBound() {
    return 1024;
  }

  /**
   * @brief Check whether a value depends on MPI rank information
   */
  bool dependsOnRank(const llvm::Value *val) const;

  /**
   * @brief Try to recover an integer range for a scalar value
   */
  bool tryEvaluateIntRange(const llvm::Value *val, int &min_value,
                           int &max_value) const;
  bool getCommunicatorSizeRange(const llvm::Value *communicator, int &min_value,
                                int &max_value) const;

private:
  llvm::Module &m_module;

  // Mapping from value to rank expression
  std::map<const llvm::Value *, RankExpr> m_value_to_rank;
  std::map<const llvm::Value *, MPIRankPredicate> m_value_to_predicate;

  // Rank at each instruction (context-sensitive)
  std::map<const llvm::Instruction *, RankExpr> m_inst_rank;
  std::map<const llvm::Instruction *, MPIRankPredicate> m_inst_predicate;
  std::map<const llvm::Instruction *, size_t> m_inst_predicate_class;
  std::map<const llvm::Instruction *, size_t> m_inst_participant_class;

  // Size of communicators
  std::map<const llvm::Value *, int> m_comm_size;
  std::map<const llvm::Value *, std::pair<int, int>> m_value_to_size_range;

  /**
   * @brief Identify MPI_Comm_rank calls
   */
  void identifyRankQueries();

  /**
   * @brief Propagate rank information through control flow
   */
  void propagateValueFacts();
  void propagateRankInfo();

  /**
   * @brief Analyze rank-dependent branches
   */
  void analyzeRankBranches();

  void canonicalizePredicateClasses();
  RankExpr mergeRankExpr(const RankExpr &lhs, const RankExpr &rhs) const;
  MPIRankPredicate mergePredicate(const MPIRankPredicate &lhs,
                                 const MPIRankPredicate &rhs) const;
  RankExpr predicateToRankExpr(const MPIRankPredicate &predicate) const;
  bool refineRankFromBranch(const llvm::BranchInst *br, unsigned succ_idx,
                            RankExpr current, RankExpr &refined) const;
  bool refinePredicateFromBranch(const llvm::BranchInst *br, unsigned succ_idx,
                                 MPIRankPredicate current,
                                 MPIRankPredicate &refined) const;
};

} // namespace MPI
