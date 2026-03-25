/**
 * @file ExpandByVal.cpp
 * @brief Expand out use of "byval" and "sret" attributes.
 *
 * This pass expands out by-value passing of structs as arguments and
 * return values. In LLVM IR terms, it expands out "byval" and "sret"
 * function argument attributes.
 *
 * NOTE: This is a simplified version for LLVM 14 migration.
 * Full implementation would need to handle actual copying of byval arguments.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/ExpandByVal.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <vector>

using namespace llvm;

namespace transform {

/**
 * @brief Run the ExpandByValPass on a module.
 *
 * Removes byval and sret attributes from all functions in the module.
 * This simplifies the IR by eliminating these special calling conventions.
 *
 * @param M The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses ExpandByValPass::run(Module &M,
                                       ModuleAnalysisManager &analysisManager) {
  bool modified = false;

  for (auto &func : M) {
    auto attrs = func.getAttributes();
    bool funcModified = false;

    // Remove byval and sret from return attributes
    auto retAttrs = attrs.getRetAttrs();
    if (retAttrs.hasAttribute(Attribute::ByVal) ||
        retAttrs.hasAttribute(Attribute::StructRet)) {
      retAttrs = retAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                     .removeAttribute(func.getContext(), Attribute::StructRet);
      funcModified = true;
    }

    // Remove byval and sret from function attributes
    auto fnAttrs = attrs.getFnAttrs();
    if (fnAttrs.hasAttribute(Attribute::ByVal) ||
        fnAttrs.hasAttribute(Attribute::StructRet)) {
      fnAttrs = fnAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                    .removeAttribute(func.getContext(), Attribute::StructRet);
      funcModified = true;
    }

    // Remove byval and sret from all argument attributes
    SmallVector<AttributeSet, 8> newArgAttrs;
    for (unsigned argIdx = 0; argIdx < func.arg_size(); ++argIdx) {
      auto argAttrs = attrs.getParamAttrs(argIdx);
      if (argAttrs.hasAttribute(Attribute::ByVal) ||
          argAttrs.hasAttribute(Attribute::StructRet)) {
        argAttrs =
            argAttrs.removeAttribute(func.getContext(), Attribute::ByVal)
                .removeAttribute(func.getContext(), Attribute::StructRet);
        funcModified = true;
      }
      newArgAttrs.push_back(argAttrs);
    }

    if (funcModified) {
      auto newAttrs =
          AttributeList::get(func.getContext(), fnAttrs, retAttrs,
                             llvm::ArrayRef<llvm::AttributeSet>(newArgAttrs));
      func.setAttributes(newAttrs);
      modified = true;
    }
  }

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
