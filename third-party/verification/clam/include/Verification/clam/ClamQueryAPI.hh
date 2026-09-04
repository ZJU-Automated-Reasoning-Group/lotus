/**
 * @file ClamQueryAPI.hh
 * @brief Query API for Clam invariant analysis
 *
 * This interface provides query methods for Clam invariant analysis,
 * including range queries and tag queries for instructions and values.
 *
 * @author Lotus Analysis Framework
 */

#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/ConstantRange.h"
#include <optional>

#include <limits>
#include <vector>

namespace llvm {
class BasicBlock;
class Instruction;
class Value;
} // namespace llvm

namespace clam {

/// @brief Query interface for Clam invariant analysis
///
/// Provides methods to query ranges and tags for instructions and values
/// during symbolic execution and invariant analysis.
class ClamQueryAPI {
public:
  /// @brief A tag is just an unsigned integer
  using TagVector = std::vector<uint64_t>;

  virtual ~ClamQueryAPI() {}

  /// @brief Check alias relationship between two memory locations
  /// @param Loc1 First memory location
  /// @param Loc2 Second memory location
  /// @param AAQI Alias analysis query info
  /// @return AliasResult indicating alias relationship
  virtual llvm::AliasResult alias(const llvm::MemoryLocation &Loc1,
                                  const llvm::MemoryLocation &Loc2,
                                  llvm::AAQueryInfo &AAQI) const = 0;

  /// @brief Get the range for the LHS of instruction I before execution
  /// @param I The instruction to query
  /// @return The constant range, or empty range if unreachable
  ///
  /// The type of I should be either integer or pointer,
  /// otherwise an error will be raised.
  virtual llvm::ConstantRange range(const llvm::Instruction &I) const = 0;

  /// @brief Get the range for the i-th operand of instruction Inst
  /// @param Inst The instruction to query
  /// @param i The operand index (0 is for LHS)
  /// @return The constant range, or empty range if unreachable
  ///
  /// The type of ArgOperand should be either integer or pointer,
  /// otherwise an error will be raised.
  virtual llvm::ConstantRange range(const llvm::Instruction &Inst,
                                    unsigned i) const = 0;

  /// @brief Get the range for value V at the entry of basic block B
  /// @param B The basic block
  /// @param V The value to query
  /// @return The constant range, or empty range if unreachable
  ///
  /// The type of V should be either integer or pointer,
  /// otherwise an error will be raised.
  virtual llvm::ConstantRange range(const llvm::BasicBlock &B,
                                    const llvm::Value &V) const = 0;

  /// @brief Get tags associated with the LHS of instruction I
  /// @param I The instruction to query
  /// @return Optional vector of tags, or None if type is not pointer
  virtual std::optional<TagVector> tags(const llvm::Instruction &I) const = 0;

  /// @brief Get tags associated with value V at entry of basic block B
  /// @param B The basic block
  /// @param V The value to query
  /// @return Optional vector of tags, or None if type is not pointer
  virtual std::optional<TagVector> tags(const llvm::BasicBlock &B,
                                         const llvm::Value &V) const = 0;
};
} // end namespace clam
