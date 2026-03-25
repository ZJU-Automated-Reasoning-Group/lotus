/**
 * @file ExplicitConsdes.h
 * @brief Pass for inserting explicit constructor/destructor calls
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_EXPLICIT_CONSDES_H
#define LOTUS_VERIFICATION_TRANSFORM_EXPLICIT_CONSDES_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class ExplicitConsdesPass
 * @brief Inserts explicit calls of module constructors and destructors
 *
 * This pass:
 * - Converts llvm.global_ctors/dtors to explicit function calls
 * - Calls constructors at the beginning of main()
 * - Calls destructors before exit() calls and return statements
 * - Important for C++ verification where global constructors/destructors matter
 */
class ExplicitConsdesPass : public llvm::ModulePass {
public:
  static char ID;

  ExplicitConsdesPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_EXPLICIT_CONSDES_H
