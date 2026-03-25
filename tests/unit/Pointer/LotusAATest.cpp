#include "TestUtils/LLVMHelpers.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>
#include <gtest/gtest.h>

#define private public
#define protected public
#include "Alias/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/LotusAA/Support/Config.h"
#undef protected
#undef private

using namespace llvm;
using lotus::unittest::parseAssembly;

namespace {

LotusAA *runLotusAA(Module &M) {
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

std::vector<CallBase *> getIndirectCalls(Function &F) {
  std::vector<CallBase *> Calls;
  for (Instruction &I : instructions(F)) {
    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (CB->isIndirectCall())
        Calls.push_back(CB);
    }
  }
  return Calls;
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

TEST(LotusAATest, AssignsDenseExplicitPseudoInputIndices) {
  const char *IR = R"(
    define void @callee(i32** %p, i32** %q) {
    entry:
      %a = load i32*, i32** %p
      %b = load i32*, i32** %q
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  std::unique_ptr<LotusAA> Pass(runLotusAA(*M));
  Function *F = M->getFunction("callee");
  ASSERT_NE(F, nullptr);

  IntraLotusAA *PTG = Pass->getPtGraph(F);
  ASSERT_NE(PTG, nullptr);
  ASSERT_EQ(PTG->getInputs().size(), 2u);

  unsigned expected_index = 0;
  for (const auto &input_item : PTG->getInputs()) {
    EXPECT_TRUE(PTG->isPseudoInput(input_item.first));
    EXPECT_EQ(PTG->getPseudoInputIndex(input_item.first),
              static_cast<int>(expected_index));
    ++expected_index;
  }
}

TEST(AliasAnalysisWrapperTest, RejectsUnimplementedAserPTAConfig) {
  const char *IR = R"(
    define i32 @test(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  lotus::AliasAnalysisWrapper Wrapper(*M, lotus::AAConfig::AserPTA_NoCtx());
  EXPECT_FALSE(Wrapper.isInitialized());
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
    IntraLotusAAConfig::lotus_restrict_output_pts = restrict_output_pts;
    IntraLotusAAConfig::lotus_memory_max_passing_func = max_passing_func;
    IntraLotusAAConfig::lotus_restrict_right_value_count = right_value_count;
    IntraLotusAAConfig::lotus_restrict_inter_structure =
        restrict_inter_structure;
  }
};

} // namespace

TEST(LotusAA, PhiMergeResolvesBothIndirectTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %sub = sub i32 %x, 1
      ret i32 %sub
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 2u);
  EXPECT_TRUE(Targets.count(M->getFunction("foo")));
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, ReturnedFunctionPointerReachesCallerIndirectCall) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 (i32)* @choose(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %cond)
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 2u);
  EXPECT_TRUE(Targets.count(M->getFunction("foo")));
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, ReturnedFunctionPointerImportsCallerLocalGuards) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 (i32)* @choose(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %cond)
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Choose = M->getFunction("choose");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Choose, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto *ChooseCall = findCallByCallee(*Main, "choose");
  ASSERT_NE(ChooseCall, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;

  Value *ChooseCond = findValueByName(*Choose, "cond");
  ASSERT_NE(ChooseCond, nullptr);

  auto FooIt = Targets.find(M->getFunction("foo"));
  auto BarIt = Targets.find(M->getFunction("bar"));
  ASSERT_NE(FooIt, Targets.end());
  ASSERT_NE(BarIt, Targets.end());

  EXPECT_TRUE(containsImportedAtom(FooIt->second));
  EXPECT_TRUE(containsImportedAtom(BarIt->second));
  EXPECT_FALSE(containsValueAtom(FooIt->second, ChooseCond, true));
  EXPECT_FALSE(containsValueAtom(BarIt->second, ChooseCond, false));
}

TEST(LotusAA, StructFieldGepKeepsNonZeroOffsetPrecision) {
  const char *IR = R"(
    %struct.S = type { i32 (i32)*, i32 (i32)* }

    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %add = add i32 %x, 42
      ret i32 %add
    }

    define i32 @main() {
    entry:
      %s = alloca %struct.S
      %f0 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      %f1 = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 1
      store i32 (i32)* @foo, i32 (i32)** %f0
      store i32 (i32)* @bar, i32 (i32)** %f1
      %loaded = load i32 (i32)*, i32 (i32)** %f1
      %res = call i32 %loaded(i32 1)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 1u);
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, VariableIndexGepUsesUnknownOffsetBucket) {
  const char *IR = R"(
    define void @main(i64 %idx) {
    entry:
      %arr = alloca [2 x i8*]
      %f0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 0
      %f1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 1
      %dyn = getelementptr inbounds [2 x i8*], [2 x i8*]* %arr, i64 0, i64 %idx
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *F0 = findValueByName(*Main, "f0");
  Value *F1 = findValueByName(*Main, "f1");
  Value *Dyn = findValueByName(*Main, "dyn");
  ASSERT_NE(F0, nullptr);
  ASSERT_NE(F1, nullptr);
  ASSERT_NE(Dyn, nullptr);

  PTResult *F0Pts = PTG->findPTResult(F0, false);
  PTResult *F1Pts = PTG->findPTResult(F1, false);
  PTResult *DynPts = PTG->findPTResult(Dyn, false);
  ASSERT_NE(F0Pts, nullptr);
  ASSERT_NE(F1Pts, nullptr);
  ASSERT_NE(DynPts, nullptr);

  PTResultIterator f0_iter(F0Pts, PTG);
  PTResultIterator f1_iter(F1Pts, PTG);
  PTResultIterator dyn_iter(DynPts, PTG);
  ASSERT_EQ(f0_iter.size(), 1);
  ASSERT_EQ(f1_iter.size(), 1);
  ASSERT_EQ(dyn_iter.size(), 1);

  EXPECT_EQ(f0_iter.begin()->first->getOffset(), 0);
  EXPECT_NE(f1_iter.begin()->first->getOffset(), 0);
  EXPECT_FALSE(PTGraph::isUnknownOffset(f1_iter.begin()->first->getOffset()));
  EXPECT_TRUE(PTGraph::isUnknownOffset(dyn_iter.begin()->first->getOffset()));
  EXPECT_NE(dyn_iter.begin()->first, f0_iter.begin()->first);
}

TEST(LotusAA, PrunedInterfaceSpillsIntoSummaryBuckets) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 0;

  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("set_cb"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->outputs.size(), 1u);
  ASSERT_GE(PTG->summary_outputs.size(), 2u);
  EXPECT_FALSE(PTG->summary_outputs[1]->empty());
}

TEST(LotusAA, WeakUpdateKeepsComplementaryPathConditions) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      %slot = alloca i8*
      store i8* %a, i8** %slot
      br i1 %cond, label %then, label %merge
    then:
      store i8* %b, i8** %slot
      br label %merge
    merge:
      %loaded = load i8*, i8** %slot
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t LoadedValues;
  PTG->getLoadValues(Load->getPointerOperand(), Load, LoadedValues);
  PTG->refineResult(LoadedValues);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  for (const auto &Item : LoadedValues) {
    if (Item.val == findValueByName(*Main, "a"))
      cond_a = Item.cond;
    else if (Item.val == findValueByName(*Main, "b"))
      cond_b = Item.cond;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_TRUE(PTG->isSatisfiable(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_NE(cond_a, cond_b);
  Value *Cond = findValueByName(*Main, "cond");
  ASSERT_NE(Cond, nullptr);
  EXPECT_TRUE(containsValueAtom(cond_a, Cond, false));
  EXPECT_TRUE(containsValueAtom(cond_b, Cond, true));
}

TEST(LotusAA, BranchNegationKeepsSiblingEdgeIdentity) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret void
    else:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);
  path_cond_t then_cond = PTG->getCFGEdgeCond(Entry, Then);
  path_cond_t else_cond = PTG->getCFGEdgeCond(Entry, Else);
  path_cond_t negated = PTG->findOrCreateNotRegion(then_cond);

  ASSERT_NE(negated, nullptr);
  EXPECT_EQ(negated->getKind(), PathCond::Kind::BranchAtom);
  EXPECT_EQ(negated, else_cond);
  EXPECT_TRUE(containsBranchAtom(negated, Entry, Else));
}

TEST(LotusAA, SameBooleanGuardAcrossControllersUsesLegacyContradictions) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      br i1 %cond, label %left_then, label %left_else
    left_then:
      ret void
    left_else:
      ret void
    right:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Left = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *LeftElse = Left->getTerminator()->getSuccessor(1);

  path_cond_t entry_true = PTG->getCFGEdgeCond(Entry, Left);
  path_cond_t left_false = PTG->getCFGEdgeCond(Left, LeftElse);
  path_cond_t combined = PTG->findOrCreateAndRegion(entry_true, left_false);

  ASSERT_NE(combined, nullptr);
  EXPECT_FALSE(PTG->isSatisfiable(combined));
}

TEST(LotusAA, SelectAndPhiRejectImpossibleBooleanMixes) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      %sel = select i1 %cond, i8* %a, i8* %b
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %sel, %then ], [ %c, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  Value *A = findValueByName(*Main, "a");
  Value *B = findValueByName(*Main, "b");
  Value *C = findValueByName(*Main, "c");
  ASSERT_NE(Phi, nullptr);
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(C, nullptr);

  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == A)
      cond_a = pt_item.second;
    else if (alloc_site == B)
      cond_b = pt_item.second;
    else if (alloc_site == C)
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_FALSE(PTG->isSatisfiable(cond_b));
  EXPECT_TRUE(PTG->isSatisfiable(cond_c));
}

TEST(LotusAA, PhiMergingSamePointerKeepsBothSiblingPathContributions) {
  const char *IR = R"(
    define i8* @main(i1 %cond) {
    entry:
      %x = alloca i8
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %x, %then ], [ %x, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  PTResultIterator iter(PhiPts, PTG);
  ASSERT_EQ(iter.size(), 1);
  path_cond_t cond = iter.begin()->second;
  EXPECT_TRUE(PTG->isSatisfiable(cond));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(cond));
  EXPECT_EQ(cond, PTG->getEmptyCond());
}

TEST(LotusAA, ComplementaryBranchOrMatchesLegacySimpleSolver) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret void
    else:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);

  path_cond_t then_cond = PTG->getCFGEdgeCond(Entry, Then);
  path_cond_t else_cond = PTG->getCFGEdgeCond(Entry, Else);
  path_cond_t combined = PTG->findOrCreateOrRegion(then_cond, else_cond);

  ASSERT_NE(combined, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(combined));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(combined));
  EXPECT_EQ(combined, PTG->getEmptyCond());
}

TEST(LotusAA, ReturnOutputsPreserveConditionalReturnSensitivity) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  for (const auto &ret_item : PTG->outputs.front()->getVal()) {
    for (const auto &mem_item : ret_item.second) {
      if (mem_item.val == findValueByName(*Main, "a"))
        cond_a = mem_item.cond;
      else if (mem_item.val == findValueByName(*Main, "b"))
        cond_b = mem_item.cond;
    }
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(cond_a));
  EXPECT_TRUE(PTG->isSatisfiable(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_NE(cond_a, cond_b);

  BasicBlock *Entry = &Main->getEntryBlock();
  BasicBlock *Then = Entry->getTerminator()->getSuccessor(0);
  BasicBlock *Else = Entry->getTerminator()->getSuccessor(1);
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_a, PTG->getCFGEdgeCond(Entry, Else))));
  EXPECT_FALSE(PTG->isSatisfiable(
      PTG->findOrCreateAndRegion(cond_b, PTG->getCFGEdgeCond(Entry, Then))));
}

TEST(LotusAA, PhiConditionsRetainNestedBranchPredicates) {
  const char *IR = R"(
    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    right:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ], [ %c, %right ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  Value *Outer = findValueByName(*Main, "outer");
  Value *Inner = findValueByName(*Main, "inner");
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == findValueByName(*Main, "a"))
      cond_a = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "b"))
      cond_b = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "c"))
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);

  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_a));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_b));
  EXPECT_FALSE(PTG->isAlwaysSatisfied(cond_c));

  EXPECT_TRUE(containsValueAtom(cond_a, Outer, true));
  EXPECT_TRUE(containsValueAtom(cond_a, Inner, true));
  EXPECT_TRUE(containsValueAtom(cond_b, Outer, true));
  EXPECT_TRUE(containsValueAtom(cond_b, Inner, false));
  EXPECT_TRUE(containsValueAtom(cond_c, Outer, false));
}

TEST(LotusAA, InterproceduralSideEffectWritesKeepCallerFunctionLevel) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 1;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 1;

  const char *IR = R"(
    %struct.S = type { i32 (i32)* }

    define void @set_cb(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      %f = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i32 (i32)* %cb, i32 (i32)** %f
      ret void
    }

    define void @wrapper(%struct.S* %s, i32 (i32)* %cb) {
    entry:
      call void @set_cb(%struct.S* %s, i32 (i32)* %cb)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Pass->computePTA(M->getFunction("set_cb"));
  Pass->computePTA(M->getFunction("wrapper"));
  auto *PTG = Pass->getPtGraph(M->getFunction("wrapper"));
  ASSERT_NE(PTG, nullptr);

  auto *Wrapper = M->getFunction("wrapper");
  ASSERT_NE(Wrapper, nullptr);
  auto *ArgS = dyn_cast<Argument>(findValueByName(*Wrapper, "s"));
  ASSERT_NE(ArgS, nullptr);

  PTResult *ArgPts = PTG->findPTResult(ArgS, false);
  ASSERT_NE(ArgPts, nullptr);
  PTResultIterator iter(ArgPts, PTG);
  ASSERT_EQ(iter.size(), 1);

  ObjectLocator *field_loc =
      iter.begin()->first->getObj()->findLocator(0, true);
  ASSERT_NE(field_loc, nullptr);
  EXPECT_EQ(field_loc->getStoreFunctionLevel(), 1);
}

TEST(LotusAA, UnknownLibraryCallPreservesLikelyThisReceiver) {
  const char *IR = R"(
    %struct.S = type { i8* }

    declare void @ext(%struct.S*, i8**)

    define i8* @main(i8* %a, i8* %b) {
    entry:
      %s = alloca %struct.S
      %slot = alloca i8*
      %field = getelementptr inbounds %struct.S, %struct.S* %s, i32 0, i32 0
      store i8* %a, i8** %field
      store i8* %b, i8** %slot
      call void @ext(%struct.S* %s, i8** %slot)
      %keep = load i8*, i8** %field
      %clobbered = load i8*, i8** %slot
      ret i8* %keep
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *KeepLoad = dyn_cast<LoadInst>(findValueByName(*Main, "keep"));
  auto *ClobLoad = dyn_cast<LoadInst>(findValueByName(*Main, "clobbered"));
  ASSERT_NE(KeepLoad, nullptr);
  ASSERT_NE(ClobLoad, nullptr);

  mem_value_t keep_values;
  PTG->getLoadValues(KeepLoad->getPointerOperand(), KeepLoad, keep_values);
  PTG->refineResult(keep_values);

  bool saw_a = false;
  for (const auto &item : keep_values) {
    if (item.val == findValueByName(*Main, "a"))
      saw_a = true;
  }
  EXPECT_TRUE(saw_a);

  mem_value_t clobbered_values;
  PTG->getLoadValues(ClobLoad->getPointerOperand(), ClobLoad, clobbered_values);
  PTG->refineResult(clobbered_values);

  bool saw_b = false;
  for (const auto &item : clobbered_values) {
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }
  EXPECT_FALSE(saw_b);
}

TEST(LotusAA, SymbolicSequentialGepCollapsesToLegacyBaseField) {
  const char *IR = R"(
    define i8* @main(i64 %idx, i8* %a, i8* %b) {
    entry:
      %arr = alloca [4 x i8*]
      %slot0 = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 0
      store i8* %a, i8** %slot0
      %sloti = getelementptr inbounds [4 x i8*], [4 x i8*]* %arr, i64 0, i64 %idx
      store i8* %b, i8** %sloti
      %loaded = load i8*, i8** %slot0
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *SlotI = findValueByName(*Main, "sloti");
  ASSERT_NE(SlotI, nullptr);
  PTResult *SlotPts = PTG->findPTResult(SlotI, false);
  ASSERT_NE(SlotPts, nullptr);

  PTResultIterator slot_iter(SlotPts, PTG);
  ASSERT_EQ(slot_iter.size(), 1u);
  EXPECT_EQ(slot_iter.begin()->first->getOffset(), 0);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_a = false;
  bool saw_b = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "a"))
      saw_a = true;
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }

  EXPECT_FALSE(saw_a);
  EXPECT_TRUE(saw_b);
}

TEST(LotusAA, DisabledLibraryHeuristicPreservesPointerArgumentValues) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_disable_library_heuristic = true;

  const char *IR = R"(
    declare void @ext(i8**)

    define i8* @main(i8* %b) {
    entry:
      %slot = alloca i8*
      store i8* %b, i8** %slot
      call void @ext(i8** %slot)
      %loaded = load i8*, i8** %slot
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_b = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "b"))
      saw_b = true;
  }
  EXPECT_TRUE(saw_b);
}

TEST(LotusAA, PthreadCreateUnknownLibraryCallStillClobbersThreadArg) {
  const char *IR = R"(
    declare i32 @pthread_create(i64*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %ctx) {
    entry:
      ret i8* %ctx
    }

    define i32 @main() {
    entry:
      %tid = alloca i64
      %slot = alloca i8*
      store i8* bitcast (i8* (i8*)* @worker to i8*), i8** %slot
      %ctx = bitcast i8** %slot to i8*
      %rc = call i32 @pthread_create(i64* %tid, i8* null, i8* (i8*)* @worker, i8* %ctx)
      %loaded = load i8*, i8** %slot
      ret i32 %rc
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);

  mem_value_t loaded_values;
  PTG->getLoadValues(Load->getPointerOperand(), Load, loaded_values);
  PTG->refineResult(loaded_values);

  bool saw_worker_ptr = false;
  for (const auto &item : loaded_values) {
    if (item.val == findValueByName(*Main, "ctx"))
      continue;
    if (auto *ce = dyn_cast<ConstantExpr>(item.val)) {
      if (ce->getOpcode() == Instruction::BitCast &&
          ce->getOperand(0) == M->getFunction("worker")) {
        saw_worker_ptr = true;
      }
    }
  }

  EXPECT_FALSE(saw_worker_ptr);
}

TEST(LotusAA, TopologicalTraversalDropsCyclicBlocksLikeLegacyTraversal) {
  const char *IR = R"(
    define void @main(i1 %cond) {
    entry:
      br label %loop
    loop:
      br i1 %cond, label %loop, label %exit
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  EXPECT_EQ(PTG->topBBs.size(), 1u);
  EXPECT_EQ(PTG->topBBs.front(), &Main->getEntryBlock());
}

TEST(LotusAA, NoValueStoresUseLegacySentinelType) {
  const char *IR = R"(
    declare void @ext(i8**)

    define void @main(i8** %slot) {
    entry:
      call void @ext(i8** %slot)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Slot = dyn_cast<Argument>(findValueByName(*Main, "slot"));
  ASSERT_NE(Slot, nullptr);
  PTResult *slot_pts = PTG->findPTResult(Slot, false);
  ASSERT_NE(slot_pts, nullptr);

  PTResultIterator iter(slot_pts, PTG);
  ASSERT_EQ(iter.size(), 1);
  MemObject *obj = iter.begin()->first->getObj();
  auto updated_it = obj->getUpdatedOffset().find(0);
  ASSERT_NE(updated_it, obj->getUpdatedOffset().end());
  EXPECT_TRUE(updated_it->second->isIntegerTy(8));
}

TEST(LotusAA, GlobalSideEffectOutputReplaysIntoCaller) {
  const char *IR = R"(
    @slot = global i8* null

    define void @set_global(i8* %p) {
    entry:
      store i8* %p, i8** @slot
      ret void
    }

    define i8* @main(i8* %a) {
    entry:
      call void @set_global(i8* %a)
      %loaded = load i8*, i8** @slot
      ret i8* %loaded
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto *Load = dyn_cast<LoadInst>(findValueByName(*Main, "loaded"));
  ASSERT_NE(Load, nullptr);
  Value *ArgA = findValueByName(*Main, "a");
  ASSERT_NE(ArgA, nullptr);

  PTResult *LoadPts = PTG->findPTResult(Load, false);
  PTResult *ArgPts = PTG->findPTResult(ArgA, false);
  ASSERT_NE(LoadPts, nullptr);
  ASSERT_NE(ArgPts, nullptr);

  PTResultIterator load_iter(LoadPts, PTG);
  PTResultIterator arg_iter(ArgPts, PTG);
  ASSERT_EQ(load_iter.size(), 1);
  ASSERT_EQ(arg_iter.size(), 1);
  EXPECT_EQ(load_iter.begin()->first, arg_iter.begin()->first);
}

TEST(LotusAA, IncompatibleIndirectTargetsDoNotConsumeCalleeIndexTwice) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_cg_size = 3;

  const char *IR = R"(
    define void @bad1(i64 %x) {
    entry:
      ret void
    }

    define void @bad2(i1 %x) {
    entry:
      ret void
    }

    define i8* @good(i8* %x) {
    entry:
      ret i8* %x
    }

    define i8* @main(i8* %arg, i8* (i8*)* %fp) {
    entry:
      %res = call i8* %fp(i8* %arg)
      ret i8* %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Good = M->getFunction("good");
  Function *Bad1 = M->getFunction("bad1");
  Function *Bad2 = M->getFunction("bad2");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Good, nullptr);
  ASSERT_NE(Bad1, nullptr);
  ASSERT_NE(Bad2, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  CallTargetSet targets;
  targets[Bad1] = nullptr;
  targets[Bad2] = nullptr;
  targets[Good] = nullptr;
  Pass->getFunctionPointerResults().setTargets(Main, Call, targets);

  Pass->computePTA(Good);
  Pass->computePTA(Main);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_TRUE(PTG->func_arg.count(Call));
  EXPECT_TRUE(PTG->func_arg[Call].count(Good));
}

TEST(LotusAA, PointerCompatibleIndirectTargetsAreAccepted) {
  const char *IR = R"(
    define i8* @good(i8* %x) {
    entry:
      ret i8* %x
    }

    define i32* @main(i32* %arg, i32* (i32*)* %fp) {
    entry:
      %res = call i32* %fp(i32* %arg)
      ret i32* %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Good = M->getFunction("good");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Good, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  CallTargetSet targets;
  targets[Good] = nullptr;
  Pass->getFunctionPointerResults().setTargets(Main, Call, targets);

  Pass->computePTA(Good);
  Pass->computePTA(Main);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  ASSERT_TRUE(PTG->func_arg.count(Call));
  EXPECT_TRUE(PTG->func_arg[Call].count(Good));
}

TEST(LotusAA, CallTargetConditionsAreExclusiveForSameIndirectCall) {
  const char *IR = R"(
    define i8* @foo(i8* %x) {
    entry:
      ret i8* %x
    }

    define i8* @bar(i8* %x) {
    entry:
      ret i8* %x
    }

    define i8* @main(i8* %arg, i8* (i8*)* %fp) {
    entry:
      %res = call i8* %fp(i8* %arg)
      ret i8* %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Foo = M->getFunction("foo");
  Function *Bar = M->getFunction("bar");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Foo, nullptr);
  ASSERT_NE(Bar, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);
  CallBase *Call = Calls.front();

  path_cond_t foo_cond = PTG->getCallTargetCond(Call->getCalledOperand(), Foo);
  path_cond_t bar_cond = PTG->getCallTargetCond(Call->getCalledOperand(), Bar);
  path_cond_t combined = PTG->findOrCreateAndRegion(foo_cond, bar_cond);

  ASSERT_NE(combined, nullptr);
  EXPECT_TRUE(PTG->isSatisfiable(foo_cond));
  EXPECT_TRUE(PTG->isSatisfiable(bar_cond));
  EXPECT_FALSE(PTG->isSatisfiable(combined));
}

TEST(LotusAA, PseudoOutputFunctionPointerFlowsToCallerIndirectCall) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define void @setfp(i1 %cond, i32 (i32)** %slot) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      store i32 (i32)* @foo, i32 (i32)** %slot
      ret void
    else:
      store i32 (i32)* @bar, i32 (i32)** %slot
      ret void
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %slot = alloca i32 (i32)*
      call void @setfp(i1 %cond, i32 (i32)** %slot)
      %fp = load i32 (i32)*, i32 (i32)** %slot
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;
  EXPECT_EQ(Targets.size(), 2u);
  EXPECT_TRUE(Targets.count(M->getFunction("foo")));
  EXPECT_TRUE(Targets.count(M->getFunction("bar")));
}

TEST(LotusAA, FullyInlineInterfaceCapsDepthAtConfiguredMaximum) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;
  IntraLotusAAConfig::lotus_restrict_summary_ap_depth = 10;
  IntraLotusAAConfig::lotus_restrict_inline_size = 100;
  IntraLotusAAConfig::lotus_restrict_ap_level = 2;

  const char *IR = R"(
    define void @noop(i8* %p) {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("noop"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->getInlineApDepth(), ::LotusConfig::MAXIMAL_SUMMARY_AP_DEPTH);
}

TEST(LotusAA, SwitchConditionsPreserveCaseSensitivity) {
  const char *IR = R"(
    define i8* @main(i32 %tag, i8* %a, i8* %b, i8* %c) {
    entry:
      switch i32 %tag, label %default [
        i32 1, label %one
        i32 2, label %two
      ]
    one:
      br label %merge
    two:
      br label %merge
    default:
      br label %merge
    merge:
      %p = phi i8* [ %a, %one ], [ %b, %two ], [ %c, %default ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  ASSERT_NE(Main, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*Main, "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  Value *Tag = findValueByName(*Main, "tag");
  ASSERT_NE(Tag, nullptr);

  path_cond_t cond_a = nullptr;
  path_cond_t cond_b = nullptr;
  path_cond_t cond_c = nullptr;

  PTResultIterator iter(PhiPts, PTG);
  for (auto &pt_item : iter) {
    Value *alloc_site = pt_item.first->getObj()->getAllocSite();
    if (alloc_site == findValueByName(*Main, "a"))
      cond_a = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "b"))
      cond_b = pt_item.second;
    else if (alloc_site == findValueByName(*Main, "c"))
      cond_c = pt_item.second;
  }

  ASSERT_NE(cond_a, nullptr);
  ASSERT_NE(cond_b, nullptr);
  ASSERT_NE(cond_c, nullptr);
  EXPECT_TRUE(containsSwitchCaseAtom(cond_a, Tag, APInt(32, 1)));
  EXPECT_TRUE(containsSwitchCaseAtom(cond_b, Tag, APInt(32, 2)));
  EXPECT_TRUE(containsSwitchDefaultAtom(cond_c, Tag));
}

TEST(LotusAA, ImportedSummaryGuardsStayCallerLocal) {
  const char *IR = R"(
    define i8* @choose(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    }

    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %then, label %else
    then:
      %call = call i8* @choose(i1 %inner, i8* %a, i8* %b)
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %call, %then ], [ %c, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Choose = M->getFunction("choose");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Choose, nullptr);

  auto *MainPTG = Pass->getPtGraph(Main);
  auto *ChoosePTG = Pass->getPtGraph(Choose);
  ASSERT_NE(MainPTG, nullptr);
  ASSERT_NE(ChoosePTG, nullptr);

  auto *Call = dyn_cast<CallBase>(findValueByName(*Main, "call"));
  ASSERT_NE(Call, nullptr);

  path_cond_t callee_cond_a = nullptr;
  path_cond_t callee_cond_b = nullptr;
  for (const auto &ret_pair : ChoosePTG->outputs.front()->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Choose, "a"))
        callee_cond_a = item.cond;
      else if (item.val == findValueByName(*Choose, "b"))
        callee_cond_b = item.cond;
    }
  }

  ASSERT_NE(callee_cond_a, nullptr);
  ASSERT_NE(callee_cond_b, nullptr);
  path_cond_t cond_a = MainPTG->importPathCond(callee_cond_a, Call, Choose);
  path_cond_t cond_b = MainPTG->importPathCond(callee_cond_b, Call, Choose);
  EXPECT_TRUE(containsImportedAtom(cond_a));
  EXPECT_TRUE(containsImportedAtom(cond_b));
  Value *Inner = findValueByName(*Main, "inner");
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(containsValueAtom(cond_a, Inner, true));
  EXPECT_FALSE(containsValueAtom(cond_b, Inner, false));
}

TEST(LotusAA, CompositeImportedSummaryGuardStaysOpaque) {
  const char *IR = R"(
    define i8* @choose(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      ret i8* %a
    else:
      ret i8* %b
    right:
      ret i8* %c
    }

    define i8* @main(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c) {
    entry:
      %call = call i8* @choose(i1 %outer, i1 %inner, i8* %a, i8* %b, i8* %c)
      ret i8* %call
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Choose = M->getFunction("choose");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Choose, nullptr);

  auto *MainPTG = Pass->getPtGraph(Main);
  auto *ChoosePTG = Pass->getPtGraph(Choose);
  ASSERT_NE(MainPTG, nullptr);
  ASSERT_NE(ChoosePTG, nullptr);

  auto *Call = dyn_cast<CallBase>(findValueByName(*Main, "call"));
  ASSERT_NE(Call, nullptr);

  path_cond_t callee_cond_a = nullptr;
  for (const auto &ret_pair : ChoosePTG->outputs.front()->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Choose, "a"))
        callee_cond_a = item.cond;
    }
  }

  ASSERT_NE(callee_cond_a, nullptr);
  ASSERT_EQ(callee_cond_a->getKind(), PathCond::Kind::And);

  path_cond_t imported = MainPTG->importPathCond(callee_cond_a, Call, Choose);
  ASSERT_NE(imported, nullptr);
  EXPECT_EQ(imported->getKind(), PathCond::Kind::ImportedAtom);
  EXPECT_EQ(imported->getImportedSource(), callee_cond_a);
  EXPECT_TRUE(containsImportedAtom(imported));

  Value *Outer = findValueByName(*Choose, "outer");
  Value *Inner = findValueByName(*Choose, "inner");
  ASSERT_NE(Outer, nullptr);
  ASSERT_NE(Inner, nullptr);
  EXPECT_FALSE(containsValueAtom(imported, Outer, true));
  EXPECT_FALSE(containsValueAtom(imported, Inner, true));
}

TEST(LotusAA, CompositeImportedFunctionSummaryStillResolvesIndirectTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @baz(i32 %x) {
    entry:
      %dec = sub i32 %x, 1
      ret i32 %dec
    }

    define i32 (i32)* @choose(i1 %outer, i1 %inner) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %else
    then:
      ret i32 (i32)* @foo
    else:
      ret i32 (i32)* @bar
    right:
      ret i32 (i32)* @baz
    }

    define i32 @main(i1 %outer, i1 %inner, i32 %x) {
    entry:
      %fp = call i32 (i32)* @choose(i1 %outer, i1 %inner)
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Main = M->getFunction("main");
  Function *Choose = M->getFunction("choose");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Choose, nullptr);
  computeAllFunctionCgs(*M, *Pass);

  auto *ChooseCall = findCallByCallee(*Main, "choose");
  ASSERT_NE(ChooseCall, nullptr);

  auto Calls = getIndirectCalls(*Main);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  const auto &Targets = It->second;

  auto FooIt = Targets.find(M->getFunction("foo"));
  auto BarIt = Targets.find(M->getFunction("bar"));
  auto BazIt = Targets.find(M->getFunction("baz"));
  ASSERT_NE(FooIt, Targets.end());
  ASSERT_NE(BarIt, Targets.end());
  ASSERT_NE(BazIt, Targets.end());

  EXPECT_TRUE(containsImportedAtom(FooIt->second));
  EXPECT_TRUE(containsImportedAtom(BarIt->second));
  EXPECT_TRUE(containsImportedAtom(BazIt->second));
}

TEST(LotusAA, CallerCgInliningResolvesTransitiveIndirectSetterTargets) {
  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define void @setfoo(i32 (i32)** %slot) {
    entry:
      store i32 (i32)* @foo, i32 (i32)** %slot
      ret void
    }

    define void @setbar(i32 (i32)** %slot) {
    entry:
      store i32 (i32)* @bar, i32 (i32)** %slot
      ret void
    }

    define void @apply(i1 %cond,
                       void (i32 (i32)**)* %setter_true,
                       void (i32 (i32)**)* %setter_false,
                       i32 (i32)** %slot) {
    entry:
      %setter = select i1 %cond,
                       void (i32 (i32)**)* %setter_true,
                       void (i32 (i32)**)* %setter_false
      call void %setter(i32 (i32)** %slot)
      ret void
    }

    define i32 @main(i1 %cond, i32 %x) {
    entry:
      %slot = alloca i32 (i32)*
      call void @apply(i1 %cond,
                       void (i32 (i32)**)* @setfoo,
                       void (i32 (i32)**)* @setbar,
                       i32 (i32)** %slot)
      %fp = load i32 (i32)*, i32 (i32)** %slot
      %res = call i32 %fp(i32 %x)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Apply = M->getFunction("apply");
  Function *Main = M->getFunction("main");
  ASSERT_NE(Apply, nullptr);
  ASSERT_NE(Main, nullptr);
  auto ApplyCalls = getIndirectCalls(*Apply);
  ASSERT_EQ(ApplyCalls.size(), 1u);

  auto *ApplyPTG = Pass->getPtGraph(Apply);
  ASSERT_NE(ApplyPTG, nullptr);
  ApplyPTG->computeCG();
  auto ApplyIt = ApplyPTG->cg_resolve_result.find(ApplyCalls.front());
  ASSERT_NE(ApplyIt, ApplyPTG->cg_resolve_result.end());
  EXPECT_TRUE(ApplyIt->second.empty());

  auto MainCalls = getIndirectCalls(*Main);
  ASSERT_EQ(MainCalls.size(), 1u);

  auto *MainPTG = Pass->getPtGraph(Main);
  ASSERT_NE(MainPTG, nullptr);
  MainPTG->computeCG();

  ApplyIt = ApplyPTG->cg_resolve_result.find(ApplyCalls.front());
  ASSERT_NE(ApplyIt, ApplyPTG->cg_resolve_result.end());
  EXPECT_EQ(ApplyIt->second.size(), 2u);
  EXPECT_TRUE(ApplyIt->second.count(M->getFunction("setfoo")));
  EXPECT_TRUE(ApplyIt->second.count(M->getFunction("setbar")));

  auto MainIt = MainPTG->cg_resolve_result.find(MainCalls.front());
  ASSERT_NE(MainIt, MainPTG->cg_resolve_result.end());
  EXPECT_TRUE(MainIt->second.empty());
}

TEST(LotusAA, PthreadCreateThreadArgCgInliningResolvesWorkerIndirectCall) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;

  const char *IR = R"(
    declare i32 @pthread_create(i64*, i8*, i8* (i8*)*, i8*)

    define void @foo() {
    entry:
      ret void
    }

    define i8* @worker(i8* %ctx) {
    entry:
      %slot = bitcast i8* %ctx to void ()**
      %fp = load void ()*, void ()** %slot
      call void %fp()
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i64
      %slot = alloca void ()*
      store void ()* @foo, void ()** %slot
      %ctx = bitcast void ()** %slot to i8*
      %rc = call i32 @pthread_create(i64* %tid, i8* null, i8* (i8*)* @worker, i8* %ctx)
      ret i32 %rc
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  computeAllFunctionCgs(*M, *Pass);

  auto *Worker = M->getFunction("worker");
  ASSERT_NE(Worker, nullptr);
  auto Calls = getIndirectCalls(*Worker);
  ASSERT_EQ(Calls.size(), 1u);

  auto *PTG = Pass->getPtGraph(Worker);
  ASSERT_NE(PTG, nullptr);
  auto It = PTG->cg_resolve_result.find(Calls.front());
  ASSERT_NE(It, PTG->cg_resolve_result.end());
  EXPECT_TRUE(It->second.count(M->getFunction("foo")));
}

TEST(LotusAA, DefaultSummaryCollectionDoesNotEmitSummaryValue) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = false;
  IntraLotusAAConfig::lotus_enable_summary_value = false;

  const char *IR = R"(
    define void @foo(i8** %p, i8* %a, i1 %cond) {
    entry:
      store i8* %a, i8** %p
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @foo(i8** %p, i8* %a, i1 false)
      br label %exit
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Foo = M->getFunction("foo");
  auto *PTG = Pass->getPtGraph(Foo);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Foo->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_a = false;
  bool saw_summary = false;
  for (auto &ret_pair : Output->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == findValueByName(*Foo, "a"))
        saw_a = true;
    }
    saw_summary |= containsSummaryValue(ret_pair.second);
  }

  EXPECT_TRUE(saw_a);
  EXPECT_FALSE(saw_summary);
}

TEST(LotusAA, EnabledSummaryCollectionEmitsSummaryValue) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = true;
  IntraLotusAAConfig::lotus_enable_summary_value = true;

  const char *IR = R"(
    define void @foo(i8** %p, i8* %a, i1 %cond) {
    entry:
      store i8* %a, i8** %p
      br i1 %cond, label %recurse, label %exit
    recurse:
      call void @foo(i8** %p, i8* %a, i1 false)
      br label %exit
    exit:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Foo = M->getFunction("foo");
  auto *PTG = Pass->getPtGraph(Foo);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Foo->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_summary = false;
  bool saw_fractional_confidence = false;
  for (auto &ret_pair : Output->getVal()) {
    for (const auto &item : ret_pair.second) {
      if (item.val == LocValue::SUMMARY_VALUE) {
        saw_summary = true;
        if (item.confidence < 1.0f)
          saw_fractional_confidence = true;
      }
    }
  }

  EXPECT_TRUE(saw_summary);
  EXPECT_TRUE(saw_fractional_confidence);
}

TEST(LotusAA, FullyAnalyzedCalleeDoesNotAddSummaryValueNoise) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_enable_score_computation = true;
  IntraLotusAAConfig::lotus_enable_summary_value = true;
  IntraLotusAAConfig::lotus_restrict_inline_depth = 2;

  const char *IR = R"(
    define void @setp(i8** %p, i8* %x) {
    entry:
      store i8* %x, i8** %p
      ret void
    }

    define void @wrapper(i8** %p, i8* %a, i8* %b) {
    entry:
      store i8* %a, i8** %p
      call void @setp(i8** %p, i8* %b)
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Wrapper = M->getFunction("wrapper");
  auto *PTG = Pass->getPtGraph(Wrapper);
  ASSERT_NE(PTG, nullptr);
  auto *Output = findOutputItem(PTG, &*Wrapper->arg_begin(), 0);
  ASSERT_NE(Output, nullptr);

  bool saw_summary = false;
  for (auto &ret_pair : Output->getVal()) {
    saw_summary |= containsSummaryValue(ret_pair.second);
  }

  EXPECT_FALSE(saw_summary);
}

TEST(LotusAA, OutputPseudoPointsToMergesDuplicateEntries) {
  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %a, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  auto &pseudo_pts = PTG->outputs.front()->getPseudoPointTo();
  ASSERT_EQ(pseudo_pts.size(), 1u);
  EXPECT_TRUE(PTG->isSatisfiable(pseudo_pts.front().first));
  EXPECT_TRUE(PTG->isAlwaysSatisfied(pseudo_pts.front().first));
}

TEST(LotusAA, PseudoOutputNodesPreserveNonFirstClassTypes) {
  const char *IR = R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  Function *Caller = M->getFunction("caller");
  Function *Callee = M->getFunction("callee");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto *PTG = Pass->getPtGraph(Caller);
  ASSERT_NE(PTG, nullptr);

  Instruction *SyntheticCallsite = Caller->getEntryBlock().getTerminator();
  ASSERT_NE(SyntheticCallsite, nullptr);

  auto *RetItem = new IntraLotusAA::OutputItem;
  RetItem->setType(Type::getVoidTy(Ctx));

  auto *AggregateItem = new IntraLotusAA::OutputItem;
  auto *StructTy = StructType::create(Ctx, "lotus.synthetic.output");
  StructTy->setBody({Type::getInt8PtrTy(Ctx), Type::getInt8PtrTy(Ctx)});
  AggregateItem->setType(StructTy);

  std::vector<IntraLotusAA::OutputItem *> Outputs = {RetItem, AggregateItem};
  std::vector<Value *> &PseudoValues =
      PTG->createPseudoOutputNodes(Outputs, SyntheticCallsite, Callee);

  ASSERT_EQ(PseudoValues.size(), 2u);
  EXPECT_EQ(PseudoValues[1]->getType(), StructTy);
}

TEST(LotusAA, RestrictInterStructureMergesRecursiveEscapes) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_inter_structure = 0;

  const char *IR = R"(
    %node = type { %node* }

    define %node* @main(i1 %cond, %node* %arg) {
    entry:
      %a = alloca %node
      %b = alloca %node
      %a.next = getelementptr inbounds %node, %node* %a, i32 0, i32 0
      %b.next = getelementptr inbounds %node, %node* %b, i32 0, i32 0
      store %node* %arg, %node** %a.next
      store %node* %arg, %node** %b.next
      br i1 %cond, label %then, label %else
    then:
      ret %node* %a
    else:
      ret %node* %b
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_EQ(PTG->escape_objs.size(), 1u);
  EXPECT_GE(PTG->real_to_pseudo_map.size(), 2u);
}

TEST(LotusAA, OutputPseudoPointsToRespectsConfiguredLimit) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_output_pts = 1;

  const char *IR = R"(
    define i8* @main(i1 %cond, i8* %a, i8* %b) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %p = phi i8* [ %a, %then ], [ %b, %else ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);
  ASSERT_FALSE(PTG->outputs.empty());

  EXPECT_TRUE(PTG->outputs.front()->getPseudoPointTo().empty());
}

TEST(LotusAA, PtResultOptimizationKeepsConfiguredTruncationSticky) {
  LotusConfigScope ConfigScope;
  const char *IR = R"(
    define i8* @main(i1 %cond) {
    entry:
      %x = alloca i8
      %y = alloca i8
      %z = alloca i8
      %w = alloca i8
      br i1 %cond, label %left, label %right
    left:
      br i1 %cond, label %then, label %mid
    then:
      br label %merge
    mid:
      br label %merge
    right:
      br i1 %cond, label %else, label %last
    else:
      br label %merge
    last:
      br label %merge
    merge:
      %p = phi i8* [ %x, %then ], [ %y, %mid ], [ %z, %else ], [ %w, %last ]
      ret i8* %p
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  Value *Phi = findValueByName(*M->getFunction("main"), "p");
  ASSERT_NE(Phi, nullptr);
  PTResult *PhiPts = PTG->findPTResult(Phi, false);
  ASSERT_NE(PhiPts, nullptr);

  PTResultIterator first_iter(PhiPts, PTG);
  ASSERT_EQ(first_iter.size(), 3);
  EXPECT_TRUE(PhiPts->derived_list.empty());

  PTResultIterator second_iter(PhiPts, PTG);
  EXPECT_EQ(second_iter.size(), 3);
}

TEST(LotusAA, RightValueTrackingRespectsConfiguredLimit) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_right_value_count = 1;

  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @bar, %else ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  Value *Fp = findValueByName(*M->getFunction("main"), "fp");
  ASSERT_NE(Fp, nullptr);

  mem_value_t values;
  PTG->trackPtrRightValue(Fp, values);
  PTG->refineResult(values);
  EXPECT_EQ(values.size(), 1u);
}

TEST(LotusAA, RightValueTrackingUsesLegacyOuterRefinementTiming) {
  LotusConfigScope ConfigScope;
  IntraLotusAAConfig::lotus_restrict_right_value_count = 2;

  const char *IR = R"(
    define i32 @foo(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @bar(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @main(i1 %outer, i1 %inner) {
    entry:
      br i1 %outer, label %left, label %right
    left:
      br i1 %inner, label %then, label %mid
    then:
      br label %merge
    mid:
      br label %merge
    right:
      br label %merge
    merge:
      %fp = phi i32 (i32)* [ @foo, %then ], [ @foo, %mid ], [ @bar, %right ]
      %res = call i32 %fp(i32 7)
      ret i32 %res
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *Main = M->getFunction("main");
  auto *Foo = M->getFunction("foo");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Foo, nullptr);

  auto *PTG = Pass->getPtGraph(Main);
  ASSERT_NE(PTG, nullptr);

  Value *Fp = findValueByName(*Main, "fp");
  ASSERT_NE(Fp, nullptr);

  mem_value_t values;
  PTG->trackPtrRightValue(Fp, values);
  PTG->refineResult(values);

  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values.front().val, Foo);
}

#ifndef NDEBUG
TEST(LotusAA, ReassigningSSAValueTriggersAssertion) {
  const char *IR = R"(
    define void @main() {
    entry:
      ret void
    }
  )";

  LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_NE(M, nullptr);

  LotusAA *Pass = runLotusAA(*M);
  auto *PTG = Pass->getPtGraph(M->getFunction("main"));
  ASSERT_NE(PTG, nullptr);

  EXPECT_DEATH(
      {
        Argument *Synthetic = new Argument(PTGraph::DEFAULT_POINTER_TYPE);
        MemObject *Obj = PTG->newObject(Synthetic, MemObject::CONCRETE);
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
        PTG->addPointsTo(Synthetic, Obj, 0, PTG->getEmptyCond());
      },
      "Re-assigning value");
}
#endif
