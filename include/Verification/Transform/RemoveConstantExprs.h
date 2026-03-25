/**
 * @file RemoveConstantExprs.h
 * @brief Pass for converting constant expressions to instructions
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REMOVE_CONSTANT_EXPRS_H
#define LOTUS_VERIFICATION_TRANSFORM_REMOVE_CONSTANT_EXPRS_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RemoveConstantExprsPass
 * @brief Converts constant expressions to instructions
 *
 * Some verification tools handle instructions better than constant expressions.
 * This pass finds all constant expressions used as operands and converts them
 * to instructions inserted before their use.
 */
class RemoveConstantExprsPass : public llvm::ModulePass {
public:
  static char ID;

  RemoveConstantExprsPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REMOVE_CONSTANT_EXPRS_H
