#include "Checker/KINT/BugDetection.h"
#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/RangeAnalysis.h"
#include "Checker/KINT/SmtMemory.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <z3++.h>

using namespace llvm;

namespace {

uint64_t getNumeralU64(const z3::expr &expr) {
  z3::expr simplified = expr.simplify();
  uint64_t value = 0;
  EXPECT_TRUE(Z3_get_numeral_uint64(simplified.ctx(), simplified, &value));
  return value;
}

z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value) {
  llvm::SmallString<64> decimal;
  value.toString(decimal, 10, /*Signed=*/false, /*formatAsCLiteral=*/false);
  Z3_sort sort = Z3_mk_bv_sort(ctx, value.getBitWidth());
  Z3_ast ast = Z3_mk_numeral(ctx, decimal.c_str(), sort);
  return z3::to_expr(ctx, ast);
}

class KINTCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source, "KINTCheckerTest");
  }
};

TEST_F(KINTCheckerTest, SmtMemoryLittleEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, true);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, true)), 0x1234u);
}

TEST_F(KINTCheckerTest, SmtMemoryBigEndianRoundTrip) {
  z3::context ctx;
  kint::SmtMemory memory(ctx, 64);
  const z3::expr addr = ctx.bv_val(0, 64);

  memory.storeBytes(addr, ctx.bv_val(0x1234, 16), 2, false);

  EXPECT_EQ(getNumeralU64(memory.loadBytes(addr, 2, false)), 0x1234u);
}

TEST_F(KINTCheckerTest, GetSinkFnsHandlesIndirectCalls) {
  const char *source = R"(
    define void @indirect(void (i32)* %fp, i32 %x) {
    entry:
      %v = add i32 %x, 1
      call void %fp(i32 %v)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("indirect");
  ASSERT_NE(F, nullptr);

  auto it = inst_begin(*F);
  ASSERT_NE(it, inst_end(*F));
  auto *add = dyn_cast<BinaryOperator>(&*it);
  ASSERT_NE(add, nullptr);

  auto sinks = kint::TaintAnalysis::get_sink_fns(add);
  EXPECT_TRUE(sinks.empty());
}

TEST_F(KINTCheckerTest, IsSinkReachableTerminatesOnCycles) {
  const char *source = R"(
    @g = global i32 0

    define void @cycle(i32 %x) {
    entry:
      store i32 %x, i32* @g
      %v = load i32, i32* @g
      %inc = add i32 %v, 1
      store i32 %inc, i32* @g
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("cycle");
  ASSERT_NE(F, nullptr);

  StoreInst *first_store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *store = dyn_cast<StoreInst>(&I)) {
      first_store = store;
      break;
    }
  }
  ASSERT_NE(first_store, nullptr);

  kint::TaintAnalysis analysis;
  SetVector<Function *> taint_funcs;
  EXPECT_FALSE(analysis.is_sink_reachable(first_store, taint_funcs));
  EXPECT_TRUE(taint_funcs.empty());
}

TEST_F(KINTCheckerTest, PropagateTaintAcrossFunctionsFindsTransitiveTaint) {
  const char *source = R"(
    declare void @__mkint_sink0(i32)

    define void @mid2(i32 %x) {
    entry:
      %v2 = add i32 %x, 1
      call void @__mkint_sink0(i32 %v2)
      ret void
    }

    define void @mid1(i32 %x) {
    entry:
      %v1 = add i32 %x, 1
      call void @mid2(i32 %v1)
      ret void
    }

    define void @__mkint_ann_source(i32 %x) {
    entry:
      call void @mid1(i32 %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *mid1 = module->getFunction("mid1");
  Function *mid2 = module->getFunction("mid2");
  ASSERT_NE(mid1, nullptr);
  ASSERT_NE(mid2, nullptr);
  EXPECT_TRUE(taint_funcs.contains(mid1));
  EXPECT_TRUE(taint_funcs.contains(mid2));
}

TEST_F(KINTCheckerTest, ArgumentSinkUseIsRecognized) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @__mkint_ann_alloc_user(i64 %n) {
    entry:
      %p = call i8* @malloc(i64 %n)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *malloc_fn = module->getFunction("malloc");
  ASSERT_NE(malloc_fn, nullptr);
  EXPECT_TRUE(taint_funcs.contains(malloc_fn));
}

TEST_F(KINTCheckerTest, BugDetectionKeepsDistinctBugTypesPerInstruction) {
  const char *source = R"(
    define i32 @div(i32 %x, i32 %y) {
    entry:
      %q = sdiv i32 %x, %y
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("div");
  ASSERT_NE(F, nullptr);

  auto *div = dyn_cast<BinaryOperator>(&*inst_begin(*F));
  ASSERT_NE(div, nullptr);

  kint::BugDetection bug_detection;
  bug_detection.recordBug(div, kint::interr::INT_OVERFLOW);
  bug_detection.recordBug(div, kint::interr::DIV_BY_ZERO);

  EXPECT_EQ(bug_detection.getBugPaths().size(), 2u);
}

TEST_F(KINTCheckerTest, WideConstantPreservesHighBits) {
  const char *source = R"(
    define i128 @wide() {
    entry:
      ret i128 18446744073709551616
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("wide");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);
  auto *ci = dyn_cast<ConstantInt>(ret->getReturnValue());
  ASSERT_NE(ci, nullptr);

  z3::context ctx;
  z3::solver solver(ctx);
  DenseMap<const Value *, llvm::Optional<z3::expr>> empty;
  kint::BugDetection bug_detection;
  z3::expr actual = bug_detection.v2sym(ci, empty, solver);
  z3::expr expected = bvValFromAPInt(ctx, ci->getValue());

  solver.add(actual != expected);
  EXPECT_EQ(solver.check(), z3::unsat);
}

TEST_F(KINTCheckerTest, CallbackRangeInitWalksToCallers) {
  const char *source = R"(
    define i32 @cb(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @mid(i32 %y) {
    entry:
      %m = call i32 @cb(i32 %y)
      ret i32 %m
    }

    define i32 @top(i32 %z) {
    entry:
      %t = call i32 @mid(i32 %z)
      ret i32 %t
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::RangeAnalysis range_analysis;
  std::map<const Function *, kint::bbrange_t> func2range_info;
  std::map<const Function *, kint::crange> func2ret_range;
  SetVector<Function *> range_analysis_funcs;
  std::map<const GlobalVariable *, kint::crange> global2range;
  std::map<const GlobalVariable *, SmallVector<kint::crange, 4>> garr2ranges;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  callback_tsrc_fn.insert("cb");
  range_analysis.init_ranges(*module, func2range_info, func2ret_range,
                             range_analysis_funcs, global2range, garr2ranges,
                             taint_funcs, callback_tsrc_fn);

  Function *top = module->getFunction("top");
  ASSERT_NE(top, nullptr);
  Argument *arg = top->getArg(0);
  ASSERT_NE(arg, nullptr);
  const auto &rng = func2range_info[top][&(top->getEntryBlock())][arg];
  EXPECT_TRUE(rng.isFullSet());
}

TEST_F(KINTCheckerTest, SwitchSuccessorRangeIsIntersected) {
  const char *source = R"(
    define i8 @__mkint_ann_switch(i8 %x) {
    entry:
      switch i8 %x, label %def [
        i8 1, label %case12
        i8 2, label %case12
      ]

    case12:
      ret i8 %x

    def:
      ret i8 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::RangeAnalysis range_analysis;
  std::map<const Function *, kint::bbrange_t> func2range_info;
  std::map<const Function *, kint::crange> func2ret_range;
  SetVector<Function *> range_analysis_funcs;
  std::map<const GlobalVariable *, kint::crange> global2range;
  std::map<const GlobalVariable *, SmallVector<kint::crange, 4>> garr2ranges;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;
  std::map<ICmpInst *, bool> impossible_branches;
  std::set<GetElementPtrInst *> gep_oob;
  DenseMap<const BasicBlock *, SetVector<const BasicBlock *>> backedges;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;

  Function *F = module->getFunction("__mkint_ann_switch");
  ASSERT_NE(F, nullptr);
  taint_funcs.insert(F);
  range_analysis.init_ranges(*module, func2range_info, func2ret_range,
                             range_analysis_funcs, global2range, garr2ranges,
                             taint_funcs, callback_tsrc_fn);
  range_analysis.range_analysis(*F, func2range_info, backedges, global2range,
                                garr2ranges, func2ret_range,
                                impossible_branches, gep_oob, func2tsrc,
                                callback_tsrc_fn, module->getDataLayout(),
                                nullptr, nullptr);

  auto bb_it = F->begin();
  ASSERT_NE(bb_it, F->end());
  ++bb_it;
  ASSERT_NE(bb_it, F->end());
  BasicBlock *case_bb = &*bb_it;
  ASSERT_NE(case_bb, nullptr);
  Argument *arg = F->getArg(0);
  ASSERT_NE(arg, nullptr);
  const auto &rng = func2range_info[F][case_bb][arg];
  EXPECT_FALSE(rng.isFullSet());
}

} // namespace
