//===-- Verification/Sifa/BlockTransferPolicy.h
//----------------------------===//
//
// Per-block transfer strategy for precision-performance trade-offs.
// When set, blocks in the "block-wise" set use a fast, imprecise transfer
// (havoc); other blocks use instruction-by-instruction transfer.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_BLOCKTRANSFERPOLICY_H
#define LOTUS_VERIFICATION_SIFA_BLOCKTRANSFERPOLICY_H

#include "llvm/IR/BasicBlock.h"

#include <functional>
#include <unordered_set>

namespace lotus {
namespace sifa {

/// Transfer strategy for a single basic block.
enum class BlockTransferStrategy {
  /// Instruction-by-instruction transfer (more precise, slower).
  InstructionWise,
  /// Block-wise transfer: treat block as black box, havoc defined values
  /// (faster, less precise).
  BlockWise,
};

/// Policy to choose transfer strategy per basic block.
/// Use block-wise for selected blocks to trade precision for performance.
class BlockTransferPolicy {
public:
  BlockTransferPolicy() = default;

  /// All blocks use \p strategy (no per-block set).
  explicit BlockTransferPolicy(BlockTransferStrategy defaultStrategy)
      : default_(defaultStrategy) {}

  /// Blocks in \p blockWiseBlocks use BlockWise; others use InstructionWise.
  BlockTransferPolicy(
      std::unordered_set<const llvm::BasicBlock *> blockWiseBlocks,
      BlockTransferStrategy defaultStrategy =
          BlockTransferStrategy::InstructionWise)
      : blockWiseBlocks_(std::move(blockWiseBlocks)),
        default_(defaultStrategy) {}

  /// Use a predicate: when \p useBlockWise(bb) is true, use BlockWise for \p
  /// bb.
  explicit BlockTransferPolicy(
      std::function<bool(const llvm::BasicBlock *)> useBlockWise)
      : predicate_(std::move(useBlockWise)),
        default_(BlockTransferStrategy::InstructionWise) {}

  BlockTransferStrategy strategyFor(const llvm::BasicBlock *bb) const {
    if (predicate_) {
      if (predicate_(bb))
        return BlockTransferStrategy::BlockWise;
      return default_;
    }
    if (blockWiseBlocks_.count(bb))
      return BlockTransferStrategy::BlockWise;
    return default_;
  }

  bool useBlockWise(const llvm::BasicBlock *bb) const {
    return strategyFor(bb) == BlockTransferStrategy::BlockWise;
  }

  void addBlockWise(llvm::BasicBlock *bb) { blockWiseBlocks_.insert(bb); }
  void clearBlockWise() { blockWiseBlocks_.clear(); }
  bool empty() const { return blockWiseBlocks_.empty() && !predicate_; }

private:
  std::unordered_set<const llvm::BasicBlock *> blockWiseBlocks_;
  std::function<bool(const llvm::BasicBlock *)> predicate_;
  BlockTransferStrategy default_ = BlockTransferStrategy::InstructionWise;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_BLOCKTRANSFERPOLICY_H
