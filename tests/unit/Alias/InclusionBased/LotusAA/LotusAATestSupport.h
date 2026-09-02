#pragma once

#include "TestUtils/LLVMHelpers.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <gtest/gtest.h>

#include <set>
#include <string>

#define private public
#define protected public
#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/InclusionBased/LotusAA/Support/Config.h"
#undef protected
#undef private

using namespace llvm;
using lotus::unittest::getIndirectCalls;
using lotus::unittest::parseAssembly;

namespace {

void initializePassInfra() {
  static bool initialized = false;
  if (initialized)
    return;

  auto &registry = *PassRegistry::getPassRegistry();
  initializeCore(registry);
  initializeAnalysis(registry);
  initializeTransformUtils(registry);
  initialized = true;
}

LotusAA *runLotusAA(Module &M) {
  initializePassInfra();
  LotusAA::setFixedCallGraphModeForTesting(false);
  auto *PM = new legacy::PassManager();
  auto *Pass = new LotusAA();
  PM->add(Pass);
  PM->run(M);
  LotusAA::clearFixedCallGraphModeForTesting();
  return Pass;
}

struct LotusParallelThreadScope {
  explicit LotusParallelThreadScope(unsigned thread_count) {
    LotusAA::setParallelThreadsForTesting(thread_count);
    LotusAA::setFixedCallGraphModeForTesting(true);
  }

  ~LotusParallelThreadScope() {
    LotusAA::clearParallelThreadsForTesting();
    LotusAA::clearFixedCallGraphModeForTesting();
  }
};

LotusAA *runLotusAA(Module &M, unsigned thread_count) {
  initializePassInfra();
  LotusParallelThreadScope scope(thread_count);
  auto *PM = new legacy::PassManager();
  auto *Pass = new LotusAA();
  PM->add(Pass);
  PM->run(M);
  return Pass;
}

LotusAA *runLotusAADefaultMode(Module &M) {
  initializePassInfra();
  auto *PM = new legacy::PassManager();
  auto *Pass = new LotusAA();
  PM->add(Pass);
  PM->run(M);
  return Pass;
}

void computeAllFunctionCgs(Module &M, LotusAA &Pass) {
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (auto *PTG = Pass.getPtGraph(&F))
      PTG->computeCG();
  }
}

CallBase *findCallByCallee(Function &F, StringRef Name) {
  for (Instruction &I : instructions(F)) {
    auto *CB = dyn_cast<CallBase>(&I);
    if (!CB)
      continue;
    Function *Callee = CB->getCalledFunction();
    if (Callee && Callee->getName() == Name)
      return CB;
  }
  return nullptr;
}

Value *findValueByName(Function &F, StringRef Name) {
  for (Argument &Arg : F.args()) {
    if (Arg.getName() == Name)
      return &Arg;
  }
  for (Instruction &I : instructions(F)) {
    if (I.getName() == Name)
      return &I;
  }
  return nullptr;
}

bool containsValueAtom(path_cond_t cond, Value *value, bool sense) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::ValueAtom ||
      cond->getKind() == PathCond::Kind::BranchAtom) {
    return cond->getValue() == value && cond->getSense() == sense;
  }

  return containsValueAtom(cond->getLhs(), value, sense) ||
         containsValueAtom(cond->getRhs(), value, sense);
}

bool containsImportedAtom(path_cond_t cond) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::ImportedAtom) {
    return true;
  }

  return containsImportedAtom(cond->getLhs()) ||
         containsImportedAtom(cond->getRhs());
}

bool containsSwitchCaseAtom(path_cond_t cond, Value *value,
                            const APInt &case_value) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::SwitchCaseAtom) {
    ConstantInt *CI = cond->getCaseValue();
    return cond->getValue() == value && CI && CI->getValue() == case_value;
  }

  return containsSwitchCaseAtom(cond->getLhs(), value, case_value) ||
         containsSwitchCaseAtom(cond->getRhs(), value, case_value);
}

bool containsSwitchDefaultAtom(path_cond_t cond, Value *value) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::SwitchDefaultAtom) {
    return cond->getValue() == value;
  }

  return containsSwitchDefaultAtom(cond->getLhs(), value) ||
         containsSwitchDefaultAtom(cond->getRhs(), value);
}

bool containsSummaryValue(const mem_value_t &values) {
  for (const auto &item : values) {
    if (item.val == LocValue::SUMMARY_VALUE)
      return true;
  }
  return false;
}

bool containsBranchAtom(path_cond_t cond, BasicBlock *block,
                        BasicBlock *successor) {
  if (!cond)
    return false;

  if (cond->getKind() == PathCond::Kind::BranchAtom) {
    return cond->getBlock() == block && cond->getSuccessor() == successor;
  }

  return containsBranchAtom(cond->getLhs(), block, successor) ||
         containsBranchAtom(cond->getRhs(), block, successor);
}



IntraLotusAA::OutputItem *findOutputItem(IntraLotusAA *PTG, Value *parent,
                                         int64_t offset) {
  for (auto *output : PTG->outputs) {
    auto &info = output->getSymbolicInfo();
    if (info.getParentPtr() == parent && info.getOffset() == offset)
      return output;
  }
  return nullptr;
}

struct LotusConfigScope {
  int inline_depth = IntraLotusAAConfig::lotus_restrict_inline_depth;
  int summary_ap_depth = IntraLotusAAConfig::lotus_restrict_summary_ap_depth;
  int inline_size = IntraLotusAAConfig::lotus_restrict_inline_size;
  int ap_level = IntraLotusAAConfig::lotus_restrict_ap_level;
  int cg_size = IntraLotusAAConfig::lotus_restrict_cg_size;
  bool disable_library_heuristic =
      IntraLotusAAConfig::lotus_disable_library_heuristic;
  bool use_full_phi_cond = IntraLotusAAConfig::lotus_use_full_phi_cond;
  bool enable_score_computation =
      IntraLotusAAConfig::lotus_enable_score_computation;
  bool enable_summary_value = IntraLotusAAConfig::lotus_enable_summary_value;
  bool enable_must_kill = IntraLotusAAConfig::lotus_enable_must_kill;
  int restrict_output_pts = IntraLotusAAConfig::lotus_restrict_output_pts;
  int max_passing_func = IntraLotusAAConfig::lotus_memory_max_passing_func;
  int right_value_count = IntraLotusAAConfig::lotus_restrict_right_value_count;
  int restrict_inter_structure =
      IntraLotusAAConfig::lotus_restrict_inter_structure;

  ~LotusConfigScope() {
    IntraLotusAAConfig::lotus_restrict_inline_depth = inline_depth;
    IntraLotusAAConfig::lotus_restrict_summary_ap_depth = summary_ap_depth;
    IntraLotusAAConfig::lotus_restrict_inline_size = inline_size;
    IntraLotusAAConfig::lotus_restrict_ap_level = ap_level;
    IntraLotusAAConfig::lotus_restrict_cg_size = cg_size;
    IntraLotusAAConfig::lotus_disable_library_heuristic =
        disable_library_heuristic;
    IntraLotusAAConfig::lotus_use_full_phi_cond = use_full_phi_cond;
    IntraLotusAAConfig::lotus_enable_score_computation =
        enable_score_computation;
    IntraLotusAAConfig::lotus_enable_summary_value = enable_summary_value;
    IntraLotusAAConfig::lotus_enable_must_kill = enable_must_kill;
    IntraLotusAAConfig::lotus_restrict_output_pts = restrict_output_pts;
    IntraLotusAAConfig::lotus_memory_max_passing_func = max_passing_func;
    IntraLotusAAConfig::lotus_restrict_right_value_count = right_value_count;
    IntraLotusAAConfig::lotus_restrict_inter_structure =
        restrict_inter_structure;
  }
};

std::set<std::string> collectFunctionNames(const FunctionGroup &group) {
  std::set<std::string> names;
  for (Function *func : group) {
    if (func)
      names.insert(std::string(func->getName()));
  }
  return names;
}

std::multiset<std::set<std::string>>
collectWaveGroups(const FunctionWave &wave) {
  std::multiset<std::set<std::string>> groups;
  for (const FunctionGroup &group : wave) {
    groups.insert(collectFunctionNames(group));
  }
  return groups;
}

} // namespace

















































#ifndef NDEBUG
#endif
