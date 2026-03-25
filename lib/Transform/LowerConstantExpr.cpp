//===- LowerConstantExpr.cpp - Convert constant expressions to instructions
//----===//
//
// This file is distributed under the MIT License. See LICENSE for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// \brief Constant expression lowering pass implementation
///
/// This pass converts constant expressions to regular instructions, which
/// can simplify analysis by eliminating complex constant expressions that
/// may be difficult to handle in downstream passes.
///
/// Constant expressions are expressions that can be computed at compile time
/// but exist as values in the IR. Converting them to instructions can help:
/// - Simplify analysis passes
/// - Make the IR more uniform
/// - Enable further optimization
///===----------------------------------------------------------------------===//

// LowerConstantExpr pass converts constant expressions to regular instructions.
// This is useful for simplifying analysis by eliminating complex constant
// expressions that may be difficult to handle in downstream passes.

#include "Transform/LowerConstantExpr.h"

#include <map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/ErrorHandling.h>

#define DEBUG_TYPE "lower-constant-expr"

// Transform an instruction by converting its constant expression operands to
// regular instructions. For PHI nodes, inserts new instructions before the
// terminator of the incoming block. For other instructions, inserts new
// instructions before the current instruction. Returns true if any
// transformation was performed.
static bool transform(Instruction &I) {
  try {
    bool Changed = false;
    auto *Phi = dyn_cast<PHINode>(&I);
    if (Phi) {
      std::map<std::pair<Value *, BasicBlock *>, Instruction *> IncomeMap;
      for (unsigned K = 0; K < Phi->getNumIncomingValues(); ++K) {
        auto *OpK = Phi->getIncomingValue(K);
        if (!OpK)
          continue; // Skip null operands

        if (auto *CE = dyn_cast<ConstantExpr>(OpK)) {
          Instruction *CEInst =
              IncomeMap[std::make_pair(CE, Phi->getIncomingBlock(K))];
          if (!CEInst) {
            try {
              CEInst = CE->getAsInstruction();
              if (!CEInst)
                continue; // Skip if we can't create an instruction

              IncomeMap[std::make_pair(CE, Phi->getIncomingBlock(K))] = CEInst;
              CEInst->insertBefore(Phi->getIncomingBlock(K)->getTerminator());
            } catch (...) {
              // If we can't create the instruction, just skip this operand
              errs() << "WARNING: Failed to convert constant expression to "
                        "instruction in PHI node\n";
              continue;
            }
          }
          transform(*CEInst);
          Phi->setIncomingValue(K, CEInst);
          if (!Changed)
            Changed = true;
        }
      }
    } else {
      for (unsigned K = 0; K < I.getNumOperands(); ++K) {
        auto *OpK = I.getOperand(K);
        if (!OpK)
          continue; // Skip null operands

        if (auto *CE = dyn_cast<ConstantExpr>(OpK)) {
          try {
            auto *CEInst = CE->getAsInstruction();
            if (!CEInst)
              continue; // Skip if we can't create an instruction

            CEInst->insertBefore(&I);
            transform(*CEInst);
            I.setOperand(K, CEInst);
            if (!Changed)
              Changed = true;
          } catch (...) {
            // If we can't create the instruction, just skip this operand
            errs() << "WARNING: Failed to convert constant expression to "
                      "instruction\n";
            continue;
          }
        }
      }
    }
    return Changed;
  } catch (const std::exception &e) {
    errs() << "WARNING: Exception in transform: " << e.what() << "\n";
    return false;
  } catch (...) {
    errs() << "WARNING: Unknown exception in transform\n";
    return false;
  }
}

// Transform a call instruction with a constant expression callee into a direct
// function call. Handles type casting for arguments and return values as
// needed. Returns true if transformation was successful.
static bool transformCall(Instruction &I, const DataLayout &DL) {
  try {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI)
      return false;
    auto *Callee = CI->getCalledFunction();
    if (Callee)
      return false;
    auto *CalledOp = CI->getCalledOperand();
    if (!CalledOp)
      return false;
    if (isa<ConstantExpr>(CalledOp)) {
      auto *PossibleCallee = CalledOp->stripPointerCastsAndAliases();
      if (!PossibleCallee)
        return false;
      if (isa<Function>(PossibleCallee)) {
        auto *FuncTy = dyn_cast<FunctionType>(
            PossibleCallee->getType()->getPointerElementType());
        if (!FuncTy)
          return false;
        if (FuncTy->getNumParams() != CI->arg_size())
          return false;

        std::vector<Value *> Args;
        for (unsigned K = 0; K < CI->arg_size(); ++K) {
          auto *ArgK = CI->getArgOperand(K);
          if (!ArgK)
            return false;

          if (!ArgK->getType() || !FuncTy->getParamType(K))
            return false;

          if (ArgK->getType() == FuncTy->getParamType(K)) {
            Args.push_back(ArgK);
          } else {
            Value *CastVal = nullptr;
            if (!ArgK->getType()->isSized() ||
                !FuncTy->getParamType(K)->isSized()) {
              return false;
            }

            if (DL.getTypeSizeInBits(ArgK->getType()) ==
                DL.getTypeSizeInBits(FuncTy->getParamType(K))) {
              CastVal = BitCastInst::CreateBitOrPointerCast(
                  ArgK, FuncTy->getParamType(K), "", &I);
            } else if (DL.getTypeSizeInBits(ArgK->getType()) >
                       DL.getTypeSizeInBits(FuncTy->getParamType(K))) {
              CastVal = TruncInst::CreateTruncOrBitCast(
                  ArgK, FuncTy->getParamType(K), "", &I);
            } else {
              CastVal = ZExtInst::CreateZExtOrBitCast(
                  ArgK, FuncTy->getParamType(K), "", &I);
            }
            Args.push_back(CastVal);
          }
        }

        auto *NewCI = CallInst::Create(FuncTy, PossibleCallee, Args, "", &I);
        NewCI->setDebugLoc(CI->getDebugLoc());

        auto *OriginalRetTy = CI->getType();
        if (OriginalRetTy == NewCI->getType()) {
          I.replaceAllUsesWith(NewCI);
        } else if (CI->getNumUses() == 0) {
          // call void bitcast (i32 (i32)* @reducerror to void (i32)*)(i32 %25)
          // #18
          //  %27 = call i32 @reducerror(i32 %25)
          // void type is not sized, and nothing to repalce.
        } else if (!OriginalRetTy->isSized() || !NewCI->getType()->isSized()) {
          // Skip if we can't determine sizes
          return false;
        } else if (DL.getTypeSizeInBits(OriginalRetTy) ==
                   DL.getTypeSizeInBits(NewCI->getType())) {
          auto *BCI =
              BitCastInst::CreateBitOrPointerCast(NewCI, OriginalRetTy, "", &I);
          I.replaceAllUsesWith(BCI);
        } else if (DL.getTypeSizeInBits(OriginalRetTy) >
                   DL.getTypeSizeInBits(NewCI->getType())) {
          // NewCI to integer, sext, pointer or bitcast
          auto *Int =
              NewCI->getType()->isIntegerTy()
                  ? (Value *)NewCI
                  : CastInst::CreateBitOrPointerCast(
                        NewCI,
                        IntegerType::get(I.getContext(), DL.getTypeSizeInBits(
                                                             NewCI->getType())),
                        "", &I);
          auto *Ext = CastInst::CreateSExtOrBitCast(
              Int,
              IntegerType::get(I.getContext(),
                               DL.getTypeSizeInBits(OriginalRetTy)),
              "", &I);
          auto *Final =
              CastInst::CreateBitOrPointerCast(Ext, OriginalRetTy, "", &I);
          I.replaceAllUsesWith(Final);
        } else {
          // DL.getTypeSizeInBits(OriginalRetTy) <
          // DL.getTypeSizeInBits(NewCI->getType()) NewCI to integer, trunc,
          // pointer or bitcast
          auto *Int =
              NewCI->getType()->isIntegerTy()
                  ? (Value *)NewCI
                  : CastInst::CreateBitOrPointerCast(
                        NewCI,
                        IntegerType::get(I.getContext(), DL.getTypeSizeInBits(
                                                             NewCI->getType())),
                        "", &I);
          auto *Ext = CastInst::CreateTruncOrBitCast(
              Int,
              IntegerType::get(I.getContext(),
                               DL.getTypeSizeInBits(OriginalRetTy)),
              "", &I);
          auto *Final =
              CastInst::CreateBitOrPointerCast(Ext, OriginalRetTy, "", &I);
          I.replaceAllUsesWith(Final);
        }
        return true;
      }
    }
    return false;
  } catch (const std::exception &e) {
    errs() << "WARNING: Exception in transformCall: " << e.what() << "\n";
    return false;
  } catch (...) {
    errs() << "WARNING: Unknown exception in transformCall\n";
    return false;
  }
}

// New Pass Manager entry point. Performs two passes over the module:
// 1. Transform call instructions with constant expression callees
// 2. Transform other instructions with constant expression operands
PreservedAnalyses LowerConstantExprPass::run(Module &M,
                                             ModuleAnalysisManager &) {
  try {
    bool Changed = false;

    // First pass: transform call instructions
    for (auto &F : M) {
      for (auto &B : F) {
        for (auto InstIt = B.begin(); InstIt != B.end();) {
          try {
            auto &Inst = *InstIt;
            auto Transformed = transformCall(Inst, M.getDataLayout());
            if (Transformed) {
              if (!Changed)
                Changed = true;
              InstIt = Inst.eraseFromParent();
            } else {
              ++InstIt;
            }
          } catch (...) {
            errs() << "WARNING: Exception while transforming call instruction, "
                      "skipping\n";
            ++InstIt;
          }
        }
      }
    }

    // Second pass: transform other constant expressions
    for (auto &F : M) {
      for (auto &B : F) {
        for (auto &I : B) {
          try {
            auto Transformed = transform(I);
            if (!Changed && Transformed)
              Changed = Transformed;
          } catch (...) {
            errs() << "WARNING: Exception while transforming instruction, "
                      "skipping\n";
          }
        }
      }
    }

    // Verify the module and stop if the transformation produced invalid IR.
    if (verifyModule(M, &errs())) {
      report_fatal_error(
          "LowerConstantExpr produced invalid IR; aborting optimization "
          "pipeline");
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  } catch (const std::exception &e) {
    errs() << "WARNING: Exception in LowerConstantExprPass::run: " << e.what()
           << "\n";
    errs() << "Continuing with analysis, but results may be incomplete.\n";
    return PreservedAnalyses::all();
  } catch (...) {
    errs() << "WARNING: Unknown exception in LowerConstantExprPass::run\n";
    errs() << "Continuing with analysis, but results may be incomplete.\n";
    return PreservedAnalyses::all();
  }
}
