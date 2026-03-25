/**
 * @file ExpandIndirectBr.cpp
 * @brief Expand out indirectbr instructions and blockaddress ConstantExprs.
 *
 * Indirectbr is used to implement computed gotos (a GNU extension to C).
 * This pass replaces indirectbr instructions with switch instructions, so that
 * later analysis passes don't have to worry about indirectbr instructions and
 * blockaddress ConstantExprs.
 *
 * The resulting switches might not be as fast as the original indirectbrs.
 * If you are compiling a program that has a compile-time option for using
 * computed gotos, it's possible that the program will run faster with the
 * option turned off than with using computed gotos + ExpandIndirectBr.
 *
 * @author rainoftime
 */
#include "Alias/TPA/Transforms/ExpandIndirectBr.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace transform {

/**
 * @brief Convert all indirectbr instructions in a function to switches.
 *
 * For each indirectbr, creates a switch instruction that maps block addresses
 * to integer labels. Also replaces all blockaddress ConstantExprs with integer
 * constants. Unused blockaddresses are replaced with dummy values.
 *
 * @param func The function to convert
 * @return true if any changes were made, false otherwise
 */
static bool convertFunction(Function *func) {
  bool isChanged = false;
  IntegerType *i32 = Type::getInt32Ty(func->getContext());

  // Skip zero in case programs treat a null pointer as special.
  uint32_t nextNum = 1;
  DenseMap<BasicBlock *, ConstantInt *> labelNums;
  BasicBlock *defaultBB = nullptr;

  // Replace each indirectbr with a switch.
  //
  // If there are multiple indirectbr instructions in the function,
  // this could be expensive.  While an indirectbr is usually
  // converted to O(1) machine instructions, the switch we generate
  // here will be O(n) in the number of target labels.
  //
  // However, Clang usually generates just a single indirectbr per
  // function anyway when compiling C computed gotos.
  //
  // We could try to generate one switch to handle all the indirectbr
  // instructions in the function, but that would be complicated to
  // implement given that variables that are live at one indirectbr
  // might not be live at others.
  for (auto &bb : *func) {
    if (IndirectBrInst *br = dyn_cast<IndirectBrInst>(bb.getTerminator())) {
      isChanged = true;

      if (defaultBB == nullptr) {
        defaultBB =
            BasicBlock::Create(func->getContext(), "indirectbr_default", func);
        new UnreachableInst(func->getContext(), defaultBB);
      }

      // An indirectbr can list the same target block multiple times.
      // Keep track of the basic blocks we've handled to avoid adding
      // the same case multiple times.
      DenseSet<BasicBlock *> blocksSeen;

      Value *cast =
          new PtrToIntInst(br->getAddress(), i32, "indirectbr_cast", br);
      unsigned count = br->getNumSuccessors();
      SwitchInst *switchInst = SwitchInst::Create(cast, defaultBB, count, br);
      for (unsigned i = 0; i < count; ++i) {
        BasicBlock *dest = br->getSuccessor(i);
        if (!blocksSeen.insert(dest).second) {
          // Remove duplicated entries from phi nodes.
          for (auto &inst : *dest) {
            PHINode *phiInst = dyn_cast<PHINode>(&inst);
            if (!phiInst)
              break;
            phiInst->removeIncomingValue(br->getParent());
          }
          continue;
        }

        ConstantInt *val;
        if (labelNums.count(dest) == 0) {
          val = ConstantInt::get(i32, nextNum++);
          labelNums[dest] = val;

          BlockAddress *ba = BlockAddress::get(func, dest);
          Value *valAsPtr = ConstantExpr::getIntToPtr(val, ba->getType());
          ba->replaceAllUsesWith(valAsPtr);
          ba->destroyConstant();
        } else {
          val = labelNums[dest];
        }
        switchInst->addCase(val, br->getSuccessor(i));
      }
      br->eraseFromParent();
    }
  }

  // If there are any blockaddresses that are never used by an
  // indirectbr, replace them with dummy values.
  SmallVector<Value *, 16> uses(func->use_begin(), func->use_end());
  for (auto *use : uses) {
    if (BlockAddress *ba = dyn_cast<BlockAddress>(use)) {
      isChanged = true;
      Value *dummyVal =
          ConstantExpr::getIntToPtr(ConstantInt::get(i32, ~0L), ba->getType());
      ba->replaceAllUsesWith(dummyVal);
      ba->destroyConstant();
    }
  }
  return isChanged;
}

/**
 * @brief Run the ExpandIndirectBr pass on a module.
 *
 * Processes all functions in the module to expand indirectbr instructions.
 *
 * @param M The module to transform
 * @param analysisManager Module analysis manager (unused)
 * @return PreservedAnalyses::none() if modified, PreservedAnalyses::all()
 * otherwise
 */
PreservedAnalyses
ExpandIndirectBr::run(Module &M, ModuleAnalysisManager &analysisManager) {
  bool modified = false;
  for (auto &f : M)
    modified |= convertFunction(&f);

  return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace transform
