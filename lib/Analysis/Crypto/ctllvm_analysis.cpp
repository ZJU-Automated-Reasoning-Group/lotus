/*
 * Core taint and alias analysis for the ctllvm pass.
 */

#include "CTInternal.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <chrono>

using namespace llvm;

namespace ctllvm {
namespace detail {

CryptoAnalysisImpl::CryptoAnalysisImpl(const CTOptions &options)
    : options_(options) {}

PreservedAnalyses CryptoAnalysisImpl::run(Module &M,
                                          ModuleAnalysisManager &MAM) {
  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  updateSecureFunctionNames();

  if (options_.user_specify) {
    specify_taint_flag_ = updateTargetValues(specified_target_values_,
                                             specified_declassified_values_);
  }

  for (Function &F : M) {
    std::chrono::high_resolution_clock::time_point start_timing;
    if (options_.time_analysis) {
      start_timing = std::chrono::high_resolution_clock::now();
    }

    if (!F.isDeclaration()) {
      statistics_.overall_functions++;
    } else {
      continue;
    }

    if (options_.soundness_mode) {
      InliningResult inline_result = recursiveInlineCalls(&F);
      if (!inline_result.succeeded()) {
        statistics_.inline_fail++;
      } else {
        statistics_.inline_success++;
      }

      if (!inline_result.succeeded()) {
        errs() << "Cannot analyze function: " << F.getName() << "\n";
      }

      if (inline_result.function && inline_result.function->use_empty()) {
        FunctionAnalysisResult result = analyzeFunction(*inline_result.function, FAM);
        if (result.error == AnalysisError::TooManyAlias) {
          statistics_.too_many_alias++;
        } else if (result.error == AnalysisError::NoConstantSize) {
          statistics_.no_constant_size++;
        }
        inline_result.function->eraseFromParent();
      }
    } else {
      (void)analyzeFunction(F, FAM);
    }

    if (options_.time_analysis) {
      auto end_timing = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
          end_timing - start_timing);
      errs() << "{"
             << "\"function\": \"" << F.getName() << "\", "
             << "\"time\": " << duration.count() << "}\n";
    }
  }

  printStatistics();
  return PreservedAnalyses::all();
}

FunctionAnalysisResult
CryptoAnalysisImpl::analyzeFunction(Function &F, FunctionAnalysisManager &FAM) {
  if (options_.debug) {
    errs() << "!!!!!!!!!!Start Analyzing: " << F.getName() << "!!!!!!!!!!\n";
  }

  FunctionAnalysisResult analysis_result;
  int violation_count = 0;
  auto &AA = FAM.getResult<AAManager>(F);
  Module &M = *F.getParent();

  SetVector<Instruction *> memory_instructions;
  SetVector<Value *> tainted_values;
  SetVector<Value *> declassified_values;

  if (options_.print_function) {
    F.print(errs());
  }

  for (auto &BB : F) {
    for (auto &I : BB) {
      if (isa<LoadInst>(I) || isa<StoreInst>(I)) {
        memory_instructions.insert(&I);
      }
      if (isa<MemCpyInst>(I) || isa<MemMoveInst>(I)) {
        if (options_.soundness_mode && !isa<ConstantInt>(I.getOperand(2))) {
          analysis_result.error = AnalysisError::NoConstantSize;
          return analysis_result;
        }
        memory_instructions.insert(&I);
      }

      if (!specify_taint_flag_) {
        continue;
      }

      updateTaintList(M, F, I, false, tainted_values, specified_target_values_);
      updateTaintList(M, F, I, true, declassified_values,
                      specified_declassified_values_);
    }
  }

  if (options_.alias_threshold >= 0 &&
      static_cast<int>(memory_instructions.size()) > options_.alias_threshold) {
    analysis_result.error = AnalysisError::TooManyAlias;
    return analysis_result;
  }

  if (options_.test_all_parameters) {
    for (auto &arg : F.args()) {
      tainted_values.insert(&arg);
    }
  }

  if (options_.debug) {
    errs() << "<--Initial Taint Values and Declassified Values START-->\n";
    for (auto &val : tainted_values) {
      errs() << "[INFO.Inital] Tainted Value: " << *val << " at line "
             << getDebugLine(val, "", F) << "\n";
    }
    for (auto &val : declassified_values) {
      errs() << "[INFO.Inital] Declassified Value: " << *val << " at line "
             << getDebugLine(val, "", F) << "\n";
    }
    errs() << "<--Initial Taint Values and Declassified Values DONE-->\n";
  }

  for (auto &Arg : tainted_values) {
    statistics_.taint_source++;

    high_values_.clear();
    low_values_.clear();
    high_mayvalues_.clear();
    low_mayvalues_.clear();

    if (options_.debug) {
      errs() << "**********Analyzing Taint Source: " << *Arg << "**********\n";
    }

    SetVector<Instruction *> tainted_instructions;
    SetVector<Instruction *> aliased_instructions;
    std::map<Instruction *, int> leak_through_cache;
    std::map<Instruction *, int> leak_through_branch;
    std::map<Instruction *, int> leak_through_variable_timing;
    std::map<Instruction *, int> may_leak_through_cache;
    std::map<Instruction *, int> may_leak_through_branch;
    std::map<Instruction *, int> may_leak_through_variable_timing;

    if (hasPointerLikeType(Arg->getType())) {
      low_values_.insert(Arg);
    } else {
      high_values_.insert(Arg);
    }

    for (auto *U : Arg->users()) {
      if (Instruction *Inst = dyn_cast<Instruction>(U)) {
        wrapMetadata(*Inst, Arg, false, true);
        tainted_instructions.insert(Inst);

        if (options_.debug) {
          int debug_line = getDebugLine(Inst, "", F);
          errs() << "[DEFUSE.Add] " << *Inst << " at line " << debug_line << "\n";
        }
      }
    }

    if (options_.debug) {
      errs() << "============Done Initial Tainting============\n";
    }

    int local_analysis_result = 0;
    defUseOnly(tainted_instructions, declassified_values);
    defUseAlias(tainted_instructions, aliased_instructions, memory_instructions, AA,
                Arg, declassified_values);
    violation_count =
        checkAndReport(Arg, F, FAM, tainted_instructions, leak_through_cache,
                       leak_through_branch, leak_through_variable_timing, 1);
    local_analysis_result =
        (local_analysis_result << 1) | (violation_count > 0 ? 1 : 0);

    if (options_.enable_may_leak || options_.soundness_mode) {
      for (auto &val : high_mayvalues_) {
        high_values_.insert(val);
      }
      for (auto &val : low_mayvalues_) {
        low_values_.insert(val);
      }

      defUseMayAlias(tainted_instructions, aliased_instructions, memory_instructions,
                     AA, Arg, declassified_values);
      violation_count =
          checkAndReport(Arg, F, FAM, aliased_instructions, may_leak_through_cache,
                         may_leak_through_branch,
                         may_leak_through_variable_timing, 2);
      local_analysis_result =
          (local_analysis_result << 2) | (violation_count > 0 ? 1 : 0);
    }

    if (local_analysis_result == 0) {
      statistics_.secure_taint_source++;
    }

    analysis_result.violation_mask |= local_analysis_result;
  }

  statistics_.analyzed_functions++;
  if (analysis_result.violation_mask == 0) {
    statistics_.secure_functions++;
  }

  if (options_.soundness_mode) {
    std::string analysis_string =
        analysis_result.violation_mask == 0 ? "proved-CT" : "proved-NCT";
    errs() << F.getName() << " is: " << analysis_string << "\n";
  }

  return analysis_result;
}

void CryptoAnalysisImpl::defUseOnly(
    SetVector<Instruction *> &taintedInstructions,
    SetVector<Value *> &declassified_values) {
  buildDependencyChain(taintedInstructions, declassified_values);
}

void CryptoAnalysisImpl::defUseAlias(
    SetVector<Instruction *> &taintedInstructions,
    SetVector<Instruction *> &aliasedInstructions,
    SetVector<Instruction *> &memoryInstructions, AAResults &AA, Value *Arg,
    SetVector<Value *> &declassified_values) {
  int prev_num = taintedInstructions.size();
  int new_num = -1;
  while (prev_num != new_num) {
    new_num = prev_num;
    findAliasedInstructions(aliasedInstructions, taintedInstructions,
                            memoryInstructions, AA, Arg, declassified_values);
    if (prev_num == static_cast<int>(taintedInstructions.size())) {
      break;
    }
    prev_num = buildDependencyChain(taintedInstructions, declassified_values);
  }
}

void CryptoAnalysisImpl::defUseMayAlias(
    SetVector<Instruction *> &taintedInstructions,
    SetVector<Instruction *> &aliasedInstructions,
    SetVector<Instruction *> &memoryInstructions, AAResults &AA, Value *Arg,
    SetVector<Value *> &declassified_values) {
  SetVector<Instruction *> subaliasedInstructions;
  int prev_num_aliased =
      buildDependencyChain(aliasedInstructions, declassified_values);
  int new_num_aliased = -1;
  while (prev_num_aliased != new_num_aliased) {
    new_num_aliased = prev_num_aliased;
    findAliasedInstructions(subaliasedInstructions, aliasedInstructions,
                            memoryInstructions, AA, Arg, declassified_values);

    for (auto &val : high_mayvalues_) {
      high_values_.insert(val);
    }
    for (auto &val : low_mayvalues_) {
      low_values_.insert(val);
    }

    if (subaliasedInstructions.empty() &&
        prev_num_aliased == static_cast<int>(aliasedInstructions.size())) {
      break;
    }
    for (auto &I : subaliasedInstructions) {
      aliasedInstructions.insert(I);
    }
    prev_num_aliased = buildDependencyChain(aliasedInstructions,
                                            declassified_values);
  }
}

int CryptoAnalysisImpl::buildDependencyChain(
    SetVector<Instruction *> &taintedInstructions,
    SetVector<Value *> &declassified_values) {
  SetVector<Instruction *> worklist(taintedInstructions.begin(),
                                    taintedInstructions.end());

  while (!worklist.empty()) {
    Instruction *I = worklist.pop_back_val();
    if (options_.debug) {
      const std::string label = high_values_.contains(I) ? "high" : "low";
      errs() << "[DEFUSE.Start] " << *I << " " << label << " at line "
             << getDebugLine(I, "", *I->getFunction()) << "\n";
    }
    const bool declassified_flag = declassified_values.contains(I);

    for (auto *U : I->users()) {
      if (auto *Inst = dyn_cast<Instruction>(U)) {
        if (declassified_flag) {
          if (options_.debug) {
            errs() << "[DEFUSE.DECLASSIFIED] " << *Inst << " at line "
                   << getDebugLine(Inst, "", *Inst->getFunction()) << "\n";
          }
          continue;
        }

        bool reevaluate_flag = wrapMetadata(*Inst, I, false);
        bool insert_result = taintedInstructions.insert(Inst) || reevaluate_flag;
        if (insert_result) {
          worklist.insert(Inst);
        }

        if (options_.debug && insert_result) {
          const std::string label = high_values_.contains(Inst) ? "high" : "low";
          errs() << "[DEFUSE.Add] " << *Inst << " " << label << " at line "
                 << getDebugLine(Inst, "", *Inst->getFunction()) << "\n";
        }
      }
    }
  }

  return taintedInstructions.size();
}

int CryptoAnalysisImpl::findAliasedInstructions(
    SetVector<Instruction *> &aliasedInstructions,
    SetVector<Instruction *> &taintedInstructions,
    SetVector<Instruction *> &memoryInstructions, AAResults &AA, Value *Arg,
    SetVector<Value *> &declassified_values) {
  SetVector<Instruction *> taintedInstructionsTemp;
  SetVector<Instruction *> aliasedInstructionsTemp;

  for (auto &I : taintedInstructions) {
    bool high_in_memcpy = false;
    Value *stored_value = nullptr;
    MemoryLocation SI_loc;
    uint64_t memcopy_size = 0;
    bool memcpy_flag = false;
    if (isa<StoreInst>(I)) {
      stored_value = cast<StoreInst>(I)->getValueOperand();
      if (!taintedInstructions.contains(dyn_cast<Instruction>(stored_value)) &&
          stored_value != Arg) {
        continue;
      }
      if (declassified_values.contains(stored_value)) {
        continue;
      }
      SI_loc = MemoryLocation::get(cast<StoreInst>(I));
    } else if (isa<MemCpyInst>(I) || isa<MemMoveInst>(I)) {
      if (isa<MemCpyInst>(I)) {
        SI_loc = MemoryLocation::getForDest(cast<MemCpyInst>(I));
      } else {
        SI_loc = MemoryLocation::getForDest(cast<MemMoveInst>(I));
      }
      memcpy_flag = true;
      Value *copy_size = I->getOperand(2);
      if (auto *const_size = dyn_cast<ConstantInt>(copy_size)) {
        memcopy_size = const_size->getZExtValue();
      }

      if (auto *src = dyn_cast<Value>(I->getOperand(1))) {
        if (taintedInstructions.contains(dyn_cast<Instruction>(src)) ||
            src == Arg) {
          high_in_memcpy = !high_values_.contains(src);
        }
      } else if (!memcopy_size) {
        high_in_memcpy = true;
      } else {
        high_in_memcpy = reasonMemcpy(*I, AA, memoryInstructions);
      }
    } else {
      continue;
    }

    for (auto &J : memoryInstructions) {
      if (taintedInstructions.contains(J) && high_values_.contains(J)) {
        continue;
      }
      MemoryLocation LI_loc;
      if (auto *LI = dyn_cast<LoadInst>(J)) {
        LI_loc = MemoryLocation::get(LI);
      } else if (auto *LI = dyn_cast<MemCpyInst>(J)) {
        LI_loc = MemoryLocation::getForSource(LI);
      } else {
        continue;
      }

      auto *LI = dyn_cast<Instruction>(J);
      if (!isPotentiallyReachable(I, LI, nullptr, nullptr)) {
        continue;
      }

      AliasResult AR = AA.alias(SI_loc, LI_loc);
      bool may_alias_memcpy = false;
      if (memcpy_flag) {
        for (uint64_t i = 0; i < memcopy_size; i++) {
          if (AR == AliasResult::MustAlias || AR == AliasResult::PartialAlias) {
            break;
          }
          if (AR == AliasResult::MayAlias) {
            may_alias_memcpy = true;
          }
          MemoryLocation new_SI_loc(SI_loc.Ptr, i);
          AR = AA.alias(new_SI_loc, LI_loc);
        }
      }

      if (memcpy_flag &&
          (AR != AliasResult::MustAlias && AR != AliasResult::PartialAlias) &&
          may_alias_memcpy) {
        AR = AliasResult::MayAlias;
      }

      if (options_.debug && AR != AliasResult::NoAlias) {
        errs() << "[Alias.Result] " << AR << " " << *I << " and " << *J
               << "\n";
      }
      if (AR == AliasResult::NoAlias) {
        continue;
      }

      SetVector<Value *> &high_values_ptr =
          (AR == AliasResult::MustAlias || AR == AliasResult::PartialAlias)
              ? high_values_
              : high_mayvalues_;
      SetVector<Value *> &low_values_ptr =
          (AR == AliasResult::MustAlias || AR == AliasResult::PartialAlias)
              ? low_values_
              : low_mayvalues_;

      if (isa<StoreInst>(I) && !J->getType()->isVoidTy()) {
        if (high_values_.contains(stored_value)) {
          high_values_ptr.insert(LI);
        } else {
          low_values_ptr.insert(LI);
        }
      } else if ((isa<MemCpyInst>(I) || isa<MemMoveInst>(I)) &&
                 !J->getType()->isVoidTy()) {
        if (high_in_memcpy) {
          high_values_ptr.insert(LI);
        } else {
          low_values_ptr.insert(LI);
        }
      }

      if (AR == AliasResult::MustAlias || AR == AliasResult::PartialAlias) {
        if (options_.debug) {
          const std::string label = (high_values_.contains(LI) ||
                                     high_values_ptr.contains(LI))
                                        ? "high"
                                        : "low";
          int debug_line = LI->getDebugLoc() ? LI->getDebugLoc().getLine() : -1;
          errs() << "[Alias.Must.Add] " << *J << " " << label << " at line "
                 << debug_line << "\n";
        }
        taintedInstructionsTemp.insert(LI);
      } else {
        if (options_.debug) {
          const std::string label = (high_values_.contains(LI) ||
                                     high_values_ptr.contains(LI))
                                        ? "high"
                                        : "low";
          int debug_line = LI->getDebugLoc() ? LI->getDebugLoc().getLine() : -1;
          errs() << "[Alias.May.Add] " << *J << " " << label << " at line "
                 << debug_line << "\n";
        }
        aliasedInstructionsTemp.insert(LI);
      }
    }
  }

  for (auto &I : taintedInstructionsTemp) {
    taintedInstructions.insert(I);
  }
  for (auto &I : aliasedInstructionsTemp) {
    aliasedInstructions.insert(I);
  }

  return aliasedInstructions.size();
}

bool CryptoAnalysisImpl::reasonMemcpy(
    Instruction &I, AliasAnalysis &AA,
    SetVector<Instruction *> &memoryInstructions) {
  if (!isa<MemCpyInst>(I) && !isa<MemMoveInst>(I)) {
    return false;
  }

  MemoryLocation memcpy_src_loc;
  if (auto *LI = dyn_cast<MemCpyInst>(&I)) {
    memcpy_src_loc = MemoryLocation::getForSource(LI);
  } else if (auto *LI = dyn_cast<MemMoveInst>(&I)) {
    memcpy_src_loc = MemoryLocation::getForSource(LI);
  }

  uint64_t memcopy_size = 0;
  Value *copy_size = I.getOperand(2);
  if (auto *const_size = dyn_cast<ConstantInt>(copy_size)) {
    memcopy_size = const_size->getZExtValue();
  }
  assert(memcopy_size != 0 && "Memcopy size is zero");

  for (auto &J : memoryInstructions) {
    auto *SI = dyn_cast<StoreInst>(J);
    if (!SI || !isPotentiallyReachable(J, &I, nullptr, nullptr)) {
      continue;
    }

    auto *stored_value = SI->getValueOperand();
    if (!stored_value || !high_values_.contains(stored_value)) {
      continue;
    }

    MemoryLocation store_loc = MemoryLocation::get(SI);
    AliasResult AR = AA.alias(memcpy_src_loc, store_loc);
    bool may_alias_memcpy = false;
    for (uint64_t i = 0; i < memcopy_size; i++) {
      if (AR == AliasResult::MustAlias || AR == AliasResult::PartialAlias) {
        return true;
      }
      if (AR == AliasResult::MayAlias) {
        may_alias_memcpy = true;
      }
      MemoryLocation new_memcpy_src_loc(memcpy_src_loc.Ptr, i);
      AR = AA.alias(new_memcpy_src_loc, store_loc);
    }
    if (options_.enable_may_leak && may_alias_memcpy) {
      return true;
    }
  }
  return false;
}

void CryptoAnalysisImpl::checkInstructionLeaks(
    SetVector<Instruction *> &taintedInstructions,
    std::map<Instruction *, int> &leak_through_cache,
    std::map<Instruction *, int> &leak_through_branch,
    std::map<Instruction *, int> &leak_through_variable_timing, Value *Arg,
    Function &F, FunctionAnalysisManager &FAM) {
  (void)Arg;
  (void)F;
  (void)FAM;

  for (auto &taintedInstr : taintedInstructions) {
    int line_number =
        taintedInstr->getDebugLoc() ? taintedInstr->getDebugLoc().getLine() : -1;
    if (isa<BranchInst>(taintedInstr) || isa<SwitchInst>(taintedInstr) ||
        isa<SelectInst>(taintedInstr)) {
      Value *Cond = nullptr;
      if (isa<BranchInst>(taintedInstr)) {
        Cond = cast<BranchInst>(taintedInstr)->getCondition();
      } else if (isa<SwitchInst>(taintedInstr)) {
        Cond = cast<SwitchInst>(taintedInstr)->getCondition();
      } else if (isa<SelectInst>(taintedInstr)) {
        Cond = cast<SelectInst>(taintedInstr)->getCondition();
      }

      if (options_.type_system) {
        if (Cond && high_values_.contains(Cond)) {
          leak_through_branch[taintedInstr] = line_number;
        }
      } else if (Cond &&
                 taintedInstructions.contains(dyn_cast<Instruction>(Cond))) {
        leak_through_branch[taintedInstr] = line_number;
      }
      continue;
    }

    if (isa<BinaryOperator>(taintedInstr)) {
      auto *BO = dyn_cast<BinaryOperator>(taintedInstr);
      if (BO->getOpcode() == Instruction::SDiv ||
          BO->getOpcode() == Instruction::UDiv) {
        if (options_.type_system) {
          if (BO && high_values_.contains(BO)) {
            leak_through_variable_timing[taintedInstr] = line_number;
          }
        } else if (BO && taintedInstructions.contains(BO)) {
          leak_through_variable_timing[taintedInstr] = line_number;
        }
      }
      continue;
    }

    if (isa<LoadInst>(taintedInstr) || isa<StoreInst>(taintedInstr)) {
      Value *Ptr = nullptr;
      if (isa<LoadInst>(taintedInstr)) {
        Ptr = cast<LoadInst>(taintedInstr)->getPointerOperand();
      } else if (isa<StoreInst>(taintedInstr)) {
        Ptr = cast<StoreInst>(taintedInstr)->getPointerOperand();
      }
      if (options_.type_system) {
        if (Ptr && high_values_.contains(Ptr)) {
          leak_through_cache[taintedInstr] = line_number;
        }
      } else if (Ptr &&
                 taintedInstructions.contains(dyn_cast<Instruction>(Ptr))) {
        leak_through_cache[taintedInstr] = line_number;
      }
    }
  }
}

bool CryptoAnalysisImpl::wrapMetadata(Instruction &I, Value *Arg, bool alias_flag,
                                      bool init_flag,
                                      Value *initial_taint_arg) {
  (void)alias_flag;
  (void)initial_taint_arg;

  if (!options_.type_system) {
    return false;
  }

  int already_high = high_values_.contains(&I) ? 1 : 0;

  if (I.getType()->isVoidTy()) {
    goto type_leave;
  }

  if (high_values_.contains(&I)) {
    goto type_leave;
  }

  if (init_flag) {
    if (hasPointerLikeType(Arg->getType())) {
      if (isa<LoadInst>(I)) {
        Value *loaded_value = dyn_cast<Value>(&I);
        if (hasPointerLikeType(loaded_value->getType())) {
          low_values_.insert(&I);
        } else {
          high_values_.insert(&I);
        }
      } else {
        low_values_.insert(&I);
      }
    } else {
      high_values_.insert(&I);
    }
    goto type_leave;
  }

  if (isa<LoadInst>(I)) {
    Value *loaded_value = dyn_cast<Value>(&I);
    if (!hasPointerLikeType(loaded_value->getType())) {
      high_values_.insert(&I);
    } else {
      low_values_.insert(&I);
    }
    goto type_leave;
  }

  if (high_values_.contains(Arg)) {
    high_values_.insert(&I);
  } else {
    low_values_.insert(&I);
  }

  if (isa<LoadInst>(I)) {
    if (!hasPointerLikeType(I.getType())) {
      high_values_.insert(&I);
    } else {
      low_values_.insert(&I);
    }
    goto type_leave;
  }

type_leave:
  int now_high = high_values_.contains(&I) ? 1 : 0;
  return now_high != already_high;
}

} // namespace detail
} // namespace ctllvm
