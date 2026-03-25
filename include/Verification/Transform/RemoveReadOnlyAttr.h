/**
 * @file RemoveReadOnlyAttr.h
 * @brief Pass for removing readonly attributes from instrumented functions
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REMOVE_READONLY_ATTR_H
#define LOTUS_VERIFICATION_TRANSFORM_REMOVE_READONLY_ATTR_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RemoveReadOnlyAttrPass
 * @brief Removes readonly attribute from functions that call instrumented
 * functions
 *
 * When checking for memory safety, LLVM may drop calls to functions with no
 * side effects. This pass removes readonly attributes from functions that call
 * instrumented functions (starting with __INSTR_), preventing LLVM from
 * optimizing them away.
 */
class RemoveReadOnlyAttrPass : public llvm::ModulePass {
public:
  static char ID;

  RemoveReadOnlyAttrPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REMOVE_READONLY_ATTR_H
