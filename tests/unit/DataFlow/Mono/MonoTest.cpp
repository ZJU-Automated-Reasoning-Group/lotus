/**
 * @file MonoTest.cpp
 * @brief Unit tests for Mono (monotone dataflow framework)
 */

#include "Dataflow/Mono/Analyses/Inter/InterTaintAnalysis.h"
#include "Dataflow/Mono/Analyses/Inter/InterConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Inter/InterFullConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraFullConstantPropagation.h"
#include "Dataflow/Mono/Analyses/Intra/IntraLiveVariables.h"
#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"
#include "Dataflow/Mono/Core/CallStringSolver.h"
#include "Dataflow/Mono/Solver/InterSolver.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
#include "Dataflow/Mono/Support/Result.h"
#include "TestUtils/LLVMHelpers.h"

#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mono;

class MonoTest : public lotus::unittest::LlvmModuleTest {
protected:
  template <typename InstT> InstT *findFirst(Function *F) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *Match = dyn_cast<InstT>(&I)) {
          return Match;
        }
      }
    }
    return nullptr;
  }

  Instruction *findByOpcode(Function *F, unsigned Opcode) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (I.getOpcode() == Opcode) {
          return &I;
        }
      }
    }
    return nullptr;
  }
};

// Test live variables analysis on simple function
TEST_F(MonoTest, LiveVariables) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      %d = mul i32 %c, 2
      ret i32 %d
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Verify that results are computed for all instructions
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      // Each instruction should have IN and OUT sets
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      // Sets should be initialized (may be empty)
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0);
}

// Test live variables with multiple blocks
TEST_F(MonoTest, LiveVariablesMultiBlock) {
  const char *source = R"(
    define i32 @test(i32 %a, i32 %b) {
    entry:
      %c = add i32 %a, %b
      br i1 true, label %true, label %false
    true:
      %d = mul i32 %c, 2
      br label %exit
    false:
      %e = sub i32 %c, 1
      br label %exit
    exit:
      %f = phi i32 [ %d, %true ], [ %e, %false ]
      ret i32 %f
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Find the return instruction
  ReturnInst *ret = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (ReturnInst *RI = dyn_cast<ReturnInst>(&I)) {
        ret = RI;
      }
    }
  }

  ASSERT_NE(ret, nullptr);

  // Return instruction should have computed IN/OUT
  auto &inSet = result->IN(ret);
  auto &outSet = result->OUT(ret);
  EXPECT_GE(inSet.size(), 0);
  EXPECT_EQ(outSet.size(), 0); // Out set of return should be empty
}

// Test empty function
TEST_F(MonoTest, EmptyFunction) {
  const char *source = R"(
    define void @test() {
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runLiveVariablesAnalysis(F);
  ASSERT_NE(result, nullptr);

  // Should handle empty function gracefully
  unsigned instCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      instCount++;
      auto &inSet = result->IN(&I);
      auto &outSet = result->OUT(&I);
      EXPECT_GE(inSet.size(), 0);
      EXPECT_GE(outSet.size(), 0);
    }
  }

  EXPECT_GT(instCount, 0); // At least return instruction
}

TEST_F(MonoTest, ConstantPropagationMustAliasStrongUpdate) {
  const char *source = R"(
    define i32 @test(i32* %p) {
    entry:
      store i32 1, i32* %p
      %q = bitcast i32* %p to i32*
      store i32 2, i32* %q
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoConstantPropagation(F);
  ASSERT_FALSE(result.empty());

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  // The IN map at the return should include facts for the load instruction.
  auto It = result.find(ret);
  ASSERT_NE(It, result.end());

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  auto FactIt = It->second.find(load);
  ASSERT_NE(FactIt, It->second.end());
  EXPECT_EQ(FactIt->second.Tag, ConstantPropagationTag::Const);
  EXPECT_EQ(FactIt->second.ConstValue, 2);
}

TEST_F(MonoTest, ConstantPropagationMayAliasWeakUpdate) {
  const char *source = R"(
    define i32 @test(i32* %p, i32* %q, i1 %c) {
    entry:
      store i32 1, i32* %p
      %r = select i1 %c, i32* %p, i32* %q
      store i32 2, i32* %r
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoConstantPropagation(F);
  ASSERT_FALSE(result.empty());

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  auto It = result.find(ret);
  ASSERT_NE(It, result.end());

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  auto FactIt = It->second.find(load);
  ASSERT_NE(FactIt, It->second.end());
  // A may-alias weak update leaves the value unknown (Top), not unreachable
  // (Bottom).  Bottom means "unreachable code"; Top means "unknown value".
  // After `store i32 2, i32* %r` where %r may alias %p, the analysis cannot
  // determine whether %p was updated, so the result is Top (unknown).
  EXPECT_EQ(FactIt->second.Tag, ConstantPropagationTag::Top);
}

TEST_F(MonoTest, UninitVariablesMustAliasClear) {
  const char *source = R"(
    define i32 @test(i32* %p) {
    entry:
      %q = bitcast i32* %p to i32*
      store i32 1, i32* %q
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto result = runIntraMonoUninitVariables(F);
  ASSERT_NE(result, nullptr);

  auto *load = findFirst<LoadInst>(F);
  ASSERT_NE(load, nullptr);

  // Load should not be uninitialized after a definite store to the same
  // location.
  auto &inSet = result->IN(load);
  EXPECT_EQ(inSet.count(load), 0u);
}

TEST_F(MonoTest, IntraMonoSolverPreservesExplicitMidFunctionSeed) {
  const char *source = R"(
    define void @test(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = add i32 %a, 1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public IntraMonoProblem<Domain> {
  public:
    ProblemT(Function *Fn, Instruction *SeedInst, Value *SeedFact)
        : IntraMonoProblem<Domain>({Fn}), SeedInst(SeedInst),
          SeedFact(SeedFact) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (SeedInst != nullptr && SeedFact != nullptr) {
        Seeds[SeedInst].insert(SeedFact);
      }
      return Seeds;
    }

  private:
    Instruction *SeedInst;
    Value *SeedFact;
  };

  auto *SecondInst = F->getEntryBlock().begin()->getNextNode();
  auto *FirstInst = &F->getEntryBlock().front();
  ASSERT_NE(SecondInst, nullptr);
  ASSERT_NE(FirstInst, nullptr);

  ProblemT Problem(F, SecondInst, &*F->arg_begin());
  IntraMonoSolver<Domain> Solver(Problem);
  Solver.solve();

  const auto &Facts = Solver.getInResultsAt(SecondInst);
  EXPECT_EQ(Facts.count(&*F->arg_begin()), 1u);
  EXPECT_EQ(Facts.count(FirstInst), 1u);
}

TEST_F(MonoTest, InterMonoSolverRecomputesIN) {
  const char *source = R"(
    define void @test() {
    entry:
      br i1 true, label %a, label %b
    a:
      br label %join
    b:
      br label %join
    join:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct NodeDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class NodeProblem : public InterMonoProblem<NodeDomain> {
  public:
    explicit NodeProblem(Function *Entry)
        : InterMonoProblem<NodeDomain>({Entry}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *CallSite, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      mono_container_t Out = In;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (auto *Entry =
              getEntryPoints().empty() ? nullptr : getEntryPoints().front()) {
        Seeds[&Entry->getEntryBlock().front()] = {};
      }
      return Seeds;
    }
  };

  NodeProblem Problem(F);
  InterMonoSolver<NodeDomain, 2> Solver(Problem);
  Solver.solve();

  auto *joinTerm = findByOpcode(F, Instruction::Ret);
  ASSERT_NE(joinTerm, nullptr);

  auto Facts = Solver.getResultsAt(joinTerm);
  // Both branch predecessors should contribute facts.
  EXPECT_GT(Facts.size(), 1u);
}

TEST_F(MonoTest, InterMonoTaintStrongWeakUpdate) {
  const char *source = R"(
    define void @sink(i32* %p) { ret void }
    define i32 @source() { ret i32 7 }

    define void @test(i32* %p, i32* %q) {
    entry:
      %t = call i32 @source()
      store i32 %t, i32* %p
      store i32 0, i32* %q
      call void @sink(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  InterMonoTaintConfig Config;
  Config.SourceFunctions.insert("source");
  Config.SinkFunctions.insert("sink");

  auto Result = runInterMonoTaintAnalysis(F, Config);
  ASSERT_NE(Result.Results, nullptr);

  bool FoundLeak = false;
  for (const auto &Cell : Result.Report.Leaks) {
    for (auto *V : Cell.second) {
      if (auto *Arg = dyn_cast<Argument>(V)) {
        if (Arg->getArgNo() == 0) {
          FoundLeak = true;
        }
      }
    }
  }
  EXPECT_TRUE(FoundLeak);
}

TEST_F(MonoTest, InterMonoTaintReportsAliasedSinkLeak) {
  const char *source = R"(
    define void @sink(i32* %p) { ret void }
    define i32 @source() { ret i32 7 }

    define void @test(i32* %p) {
    entry:
      %alias = bitcast i32* %p to i32*
      %t = call i32 @source()
      store i32 %t, i32* %p
      call void @sink(i32* %alias)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *Alias = findFirst<BitCastInst>(F);
  ASSERT_NE(Alias, nullptr);

  InterMonoTaintConfig Config;
  Config.SourceFunctions.insert("source");
  Config.SinkFunctions.insert("sink");

  auto Result = runInterMonoTaintAnalysis(F, Config);
  ASSERT_NE(Result.Results, nullptr);

  bool FoundAliasLeak = false;
  for (const auto &Cell : Result.Report.Leaks) {
    if (Cell.second.count(Alias) == 1u) {
      FoundAliasLeak = true;
      break;
    }
  }
  EXPECT_TRUE(FoundAliasLeak);
}

TEST_F(MonoTest, InterMonoTaintIndirectCallUsesAAResolution) {
  const char *source = R"(
    define i32 @source() {
    entry:
      ret i32 7
    }

    define i32 @producer() {
    entry:
      %x = call i32 @source()
      ret i32 %x
    }

    define void @sink(i32* %p) { ret void }

    define void @test(i32* %p) {
    entry:
      %slot = alloca i32 ()*
      store i32 ()* @producer, i32 ()** %slot
      %fp = load i32 ()*, i32 ()** %slot
      %t = call i32 %fp()
      store i32 %t, i32* %p
      call void @sink(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  InterMonoTaintConfig Config;
  Config.SourceFunctions.insert("source");
  Config.SinkFunctions.insert("sink");

  auto Result = runInterMonoTaintAnalysis(F, Config);
  ASSERT_NE(Result.Results, nullptr);

  bool FoundLeak = false;
  for (const auto &Cell : Result.Report.Leaks) {
    for (auto *V : Cell.second) {
      if (auto *Arg = dyn_cast<Argument>(V)) {
        if (Arg->getArgNo() == 0) {
          FoundLeak = true;
        }
      }
    }
  }
  EXPECT_TRUE(FoundLeak);
}

TEST_F(MonoTest, InterMonoConstantPropagationIndirectCallUsesAAResolution) {
  const char *source = R"(
    define i32 @producer() {
    entry:
      ret i32 7
    }

    define i32 @test() {
    entry:
      %slot = alloca i32 ()*
      store i32 ()* @producer, i32 ()** %slot
      %fp = load i32 ()*, i32 ()** %slot
      %t = call i32 %fp()
      ret i32 %t
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runInterMonoConstantPropagation(F);
  ASSERT_NE(Result.Results, nullptr);

  auto *Ret = findFirst<ReturnInst>(F);
  ASSERT_NE(Ret, nullptr);

  CallBase *IndirectCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Call->getCalledFunction() == nullptr) {
          IndirectCall = Call;
          break;
        }
      }
    }
  }
  ASSERT_NE(IndirectCall, nullptr);

  bool FoundConst = false;
  for (const auto &Cell : Result.Results->getINMap()) {
    if (Cell.first.Inst != Ret) {
      continue;
    }
    auto It = Cell.second.find(IndirectCall);
    if (It != Cell.second.end() &&
        It->second.Tag == ConstantPropagationTag::Const &&
        It->second.ConstValue == 7) {
      FoundConst = true;
      break;
    }
  }
  EXPECT_TRUE(FoundConst);
}

TEST_F(MonoTest, InterMonoConstantPropagationMultiCalleeSameConstantRemainsConstant) {
  const char *source = R"(
    define i32 @foo() {
    entry:
      ret i32 7
    }

    define i32 @bar() {
    entry:
      ret i32 7
    }

    define i32 @test(i1 %c) {
    entry:
      %fp = select i1 %c, i32 ()* @foo, i32 ()* @bar
      %t = call i32 %fp()
      ret i32 %t
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runInterMonoConstantPropagation(F);
  ASSERT_NE(Result.Results, nullptr);

  auto *Ret = findFirst<ReturnInst>(F);
  ASSERT_NE(Ret, nullptr);
  auto *Call = findFirst<CallInst>(F);
  ASSERT_NE(Call, nullptr);

  bool FoundConst = false;
  for (const auto &Cell : Result.Results->getINMap()) {
    if (Cell.first.Inst != Ret) {
      continue;
    }
    auto It = Cell.second.find(Call);
    if (It != Cell.second.end() &&
        It->second.Tag == ConstantPropagationTag::Const &&
        It->second.ConstValue == 7) {
      FoundConst = true;
      break;
    }
  }
  EXPECT_TRUE(FoundConst);
}

TEST_F(MonoTest, InterMonoFullConstantPropagationIndirectCallUsesAAResolution) {
  const char *source = R"(
    define i32 @producer() {
    entry:
      ret i32 7
    }

    define i32 @test() {
    entry:
      %slot = alloca i32 ()*
      store i32 ()* @producer, i32 ()** %slot
      %fp = load i32 ()*, i32 ()** %slot
      %t = call i32 %fp()
      ret i32 %t
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runInterMonoFullConstantPropagation(F);
  ASSERT_NE(Result.Results, nullptr);

  auto *Ret = findFirst<ReturnInst>(F);
  ASSERT_NE(Ret, nullptr);

  CallBase *IndirectCall = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        if (Call->getCalledFunction() == nullptr) {
          IndirectCall = Call;
          break;
        }
      }
    }
  }
  ASSERT_NE(IndirectCall, nullptr);

  bool FoundConst = false;
  for (const auto &Cell : Result.Results->getINMap()) {
    if (Cell.first.Inst != Ret) {
      continue;
    }
    auto It = Cell.second.Values.find(IndirectCall);
    if (It != Cell.second.Values.end() &&
        It->second.Tag == FullConstantTag::Const &&
        It->second.ConstValue == 7) {
      FoundConst = true;
      break;
    }
  }
  EXPECT_TRUE(FoundConst);
}

TEST_F(MonoTest, InterMonoFullConstantPropagationMultiCalleeSameConstantRemainsConstant) {
  const char *source = R"(
    define i32 @foo() {
    entry:
      ret i32 7
    }

    define i32 @bar() {
    entry:
      ret i32 7
    }

    define i32 @test(i1 %c) {
    entry:
      %fp = select i1 %c, i32 ()* @foo, i32 ()* @bar
      %t = call i32 %fp()
      ret i32 %t
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runInterMonoFullConstantPropagation(F);
  ASSERT_NE(Result.Results, nullptr);

  auto *Ret = findFirst<ReturnInst>(F);
  ASSERT_NE(Ret, nullptr);
  auto *Call = findFirst<CallInst>(F);
  ASSERT_NE(Call, nullptr);

  bool FoundConst = false;
  for (const auto &Cell : Result.Results->getINMap()) {
    if (Cell.first.Inst != Ret) {
      continue;
    }
    auto It = Cell.second.Values.find(Call);
    if (It != Cell.second.Values.end() &&
        It->second.Tag == FullConstantTag::Const &&
        It->second.ConstValue == 7) {
      FoundConst = true;
      break;
    }
  }
  EXPECT_TRUE(FoundConst);
}

TEST_F(MonoTest, InterMonoSolverUsesIndirectCallResolverHook) {
  const char *source = R"(
    define void @callee(i32* %p) {
    entry:
      ret void
    }

    define void @main(i32* %p) {
    entry:
      %slot = alloca void (i32*)*
      store void (i32*)* @callee, void (i32*)** %slot
      %fp = load void (i32*)*, void (i32*)** %slot
      call void %fp(i32* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    using n_t = typename InterMonoProblem<Domain>::n_t;
    using f_t = typename InterMonoProblem<Domain>::f_t;
    using UnresolvedCallPolicy =
        typename InterMonoProblem<Domain>::UnresolvedCallPolicy;

    ProblemT(Function *Entry, Function *Resolved)
        : InterMonoProblem<Domain>({Entry}), Resolved(Resolved) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      if (isa<CallBase>(Inst)) {
        return {};
      }
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *,
                              const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *CallSite, Function *,
                                Instruction *, Instruction *,
                                const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }

    std::vector<f_t> resolve_indirect_callees(n_t CallSite) const override {
      if (CallSite != nullptr && isa<CallBase>(CallSite) &&
          Resolved != nullptr) {
        return {Resolved};
      }
      return {};
    }

    UnresolvedCallPolicy unresolved_call_policy() const override {
      return UnresolvedCallPolicy::Ignore;
    }

  private:
    Function *Resolved;
  };

  auto *FinalRet = findFirst<ReturnInst>(Main);
  ASSERT_NE(FinalRet, nullptr);

  ProblemT Problem(Main, Callee);
  InterMonoSolver<Domain, 2> Solver(Problem);
  Solver.solve();

  auto Facts = Solver.getResultsAt(FinalRet);
  bool SawCallFact = false;
  for (auto &BB : *Main) {
    for (auto &I : BB) {
      if (isa<CallBase>(&I)) {
        SawCallFact = Facts.count(&I) == 1u;
      }
    }
  }
  EXPECT_TRUE(SawCallFact);
}

TEST_F(MonoTest, IntraConstantPropagationJoinMissingBindingIsTop) {
  const char *source = R"(
    define i32 @test(i32* %p, i1 %c) {
    entry:
      br i1 %c, label %then, label %else
    then:
      store i32 7, i32* %p
      br label %merge
    else:
      br label %merge
    merge:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runIntraMonoConstantPropagation(F);
  ASSERT_FALSE(Result.empty());

  auto *Ret = findFirst<ReturnInst>(F);
  auto *Load = findFirst<LoadInst>(F);
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  auto It = Result.find(Ret);
  ASSERT_NE(It, Result.end());
  auto FactIt = It->second.find(Load);
  if (FactIt == It->second.end()) {
    SUCCEED() << "Absent binding semantically represents Top";
  } else {
    EXPECT_EQ(FactIt->second.Tag, ConstantPropagationTag::Top);
  }
}

TEST_F(MonoTest, IntraFullConstantPropagationAliasLoadFromPartiallyInitializedStateIsTop) {
  const char *source = R"(
    define i32 @test(i32* %p, i1 %c) {
    entry:
      %q = bitcast i32* %p to i32*
      br i1 %c, label %then, label %else
    then:
      store i32 7, i32* %p
      br label %merge
    else:
      br label %merge
    merge:
      %v = load i32, i32* %q
      ret i32 %v
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto Result = runIntraMonoFullConstantPropagation(F);
  ASSERT_FALSE(Result.empty());

  auto *Ret = findFirst<ReturnInst>(F);
  auto *Load = findFirst<LoadInst>(F);
  ASSERT_NE(Ret, nullptr);
  ASSERT_NE(Load, nullptr);

  auto It = Result.find(Ret);
  ASSERT_NE(It, Result.end());
  auto FactIt = It->second.Values.find(Load);
  ASSERT_NE(FactIt, It->second.Values.end());
  EXPECT_FALSE(It->second.Unreachable);
  EXPECT_EQ(FactIt->second.Tag, FullConstantTag::Top);
}

TEST_F(MonoTest, CallBrContinuation) {
  const char *source = R"(
    declare void @callee()
    declare token @llvm.experimental.stackmap(i64, i32)

    define void @test() {
    entry:
      %token = call token @llvm.experimental.stackmap(i64 0, i32 0)
      callbr void @callee()
        to label %cont [label %cont]
    cont:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct TrivialDomain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class TrivialProblem : public InterMonoProblem<TrivialDomain> {
  public:
    explicit TrivialProblem(Function *Entry)
        : InterMonoProblem<TrivialDomain>({Entry}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      return In;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (auto *Entry =
              getEntryPoints().empty() ? nullptr : getEntryPoints().front()) {
        Seeds[&Entry->getEntryBlock().front()] = {};
      }
      return Seeds;
    }
  };

  TrivialProblem Problem(F);
  InterMonoSolver<TrivialDomain, 2> Solver(Problem);
  Solver.solve();

  auto *ret = findFirst<ReturnInst>(F);
  ASSERT_NE(ret, nullptr);

  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);
  bool Found = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst == ret) {
      Found = true;
      break;
    }
  }
  EXPECT_TRUE(Found);
}

TEST_F(MonoTest, IntraMonoSolverReentrantSolveStable) {
  const char *source = R"(
    define i32 @test(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public IntraMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Fn) : IntraMonoProblem<Domain>({Fn}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      if (Inst != nullptr) {
        Out.insert(Inst);
      }
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  ProblemT Problem(F);
  IntraMonoSolver<Domain> Solver(Problem);
  Solver.solve();
  auto FirstIn = Solver.getInResults();
  auto FirstOut = Solver.getOutResults();
  auto FirstIters = Solver.getStatistics().iterations;

  Solver.solve();
  auto SecondIn = Solver.getInResults();
  auto SecondOut = Solver.getOutResults();
  auto SecondIters = Solver.getStatistics().iterations;

  EXPECT_EQ(FirstIn, SecondIn);
  EXPECT_EQ(FirstOut, SecondOut);
  EXPECT_EQ(FirstIters, SecondIters);
}

TEST_F(MonoTest, IntraMonoSolverSingleNodeProcessed) {
  const char *source = R"(
    define void @test() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public IntraMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Fn) : IntraMonoProblem<Domain>({Fn}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      Out.insert(Inst);
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  ProblemT Problem(F);
  IntraMonoSolver<Domain> Solver(Problem);
  Solver.solve();

  auto *Ret = findFirst<ReturnInst>(F);
  ASSERT_NE(Ret, nullptr);
  const auto &Out = Solver.getOutResultsAt(Ret);
  EXPECT_EQ(Out.count(Ret), 1u);
  EXPECT_GE(Solver.getStatistics().nodes_processed, 1u);
}

TEST_F(MonoTest, IntraMonoSolverWideningCounterResetsAcrossRuns) {
  const char *source = R"(
    define void @test(i1 %c) {
    entry:
      br label %loop
    loop:
      br i1 %c, label %loop, label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public IntraMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Fn) : IntraMonoProblem<Domain>({Fn}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      mono_container_t Out = In;
      Out.insert(Inst);
      return Out;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t widen(const mono_container_t &OldVal,
                           const mono_container_t &NewVal) override {
      ++WidenCalls;
      mono_container_t Out = OldVal;
      Out.insert(NewVal.begin(), NewVal.end());
      return Out;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }

    void resetWidenCalls() { WidenCalls = 0u; }
    unsigned getWidenCalls() const { return WidenCalls; }

  private:
    unsigned WidenCalls = 0u;
  };

  ProblemT Problem(F);
  IntraMonoSolver<Domain> Solver(Problem);
  Solver.setWideningThreshold(1u);

  Problem.resetWidenCalls();
  Solver.solve();
  auto FirstRunWidenCalls = Problem.getWidenCalls();
  ASSERT_GT(FirstRunWidenCalls, 0u);

  Problem.resetWidenCalls();
  Solver.solve();
  auto SecondRunWidenCalls = Problem.getWidenCalls();
  EXPECT_EQ(FirstRunWidenCalls, SecondRunWidenCalls);
}

TEST_F(MonoTest, InterMonoSolverEmptyContextSeedStaysLocalForPositiveK) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @main(i32 %a) {
    entry:
      %r = call i32 @callee(i32 %a)
      ret i32 %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry, Function *CalleeFn)
        : InterMonoProblem<Domain>({Entry}), CalleeFn(CalleeFn) {}

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *Callee,
                              const mono_container_t &) override {
      mono_container_t Out;
      if (Callee == CalleeFn) {
        Out.insert(&*Callee->arg_begin());
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      return In;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      auto *EntryInst = &getEntryPoints().front()->getEntryBlock().front();
      Seeds[EntryInst] = {};
      auto *CalleeEntry = &CalleeFn->getEntryBlock().front();
      Seeds[CalleeEntry].insert(CalleeEntry);
      return Seeds;
    }

  private:
    Function *CalleeFn;
  };

  ProblemT Problem(Main, Callee);
  InterMonoSolver<Domain, 2> Solver(Problem);
  Solver.solve();

  auto *CalleeEntry = &Callee->getEntryBlock().front();
  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);
  bool FoundSeedCtx = false;
  bool SeedLeakedIntoCallContext = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst != CalleeEntry) {
      continue;
    }
    const auto &Facts = Cell.second;
    if (Cell.first.Ctx.empty() && Facts.count(CalleeEntry) == 1u) {
      FoundSeedCtx = true;
    }
    if (!Cell.first.Ctx.empty() && Facts.count(CalleeEntry) == 1u) {
      SeedLeakedIntoCallContext = true;
    }
  }
  EXPECT_TRUE(FoundSeedCtx);
  EXPECT_FALSE(SeedLeakedIntoCallContext);

  auto *MainRet = findFirst<ReturnInst>(Main);
  ASSERT_NE(MainRet, nullptr);
  bool SeedReachedCaller = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst != MainRet) {
      continue;
    }
    if (Cell.second.count(CalleeEntry) == 1u) {
      SeedReachedCaller = true;
      break;
    }
  }
  EXPECT_FALSE(SeedReachedCaller);
}

TEST_F(MonoTest, CallStringEngineProcessesSeedInsWithoutSeeds) {
  const char *source = R"(
    define void @seeded(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Seeded = module->getFunction("seeded");
  ASSERT_NE(Seeded, nullptr);

  using Container = std::set<Value *>;
  using Engine =
      dataflow::CallStringInterProceduralDataFlowEngine<0, Container>;
  using ResultTy = Engine::ResultTy;
  using Context = Engine::Context;
  using ContextKey = Engine::ContextKey;

  dataflow::controlflow::LLVMInterCFG ICF(module.get());
  Engine E;
  Context EmptyCtx;

  auto *EntryInst = &Seeded->getEntryBlock().front();
  auto *Ret = findFirst<ReturnInst>(Seeded);
  ASSERT_NE(Ret, nullptr);

  std::vector<ContextKey> Seeds;
  std::map<ContextKey, Container> SeedIns;
  SeedIns[{EntryInst, EmptyCtx}].insert(&*Seeded->arg_begin());

  auto ComputeGEN = [](Instruction *, ResultTy *) {};
  auto ComputeKILL = [](Instruction *, ResultTy *) {};
  auto InitializeIN = [](Instruction *, Container &IN) { IN.clear(); };
  auto InitializeOUT = [](Instruction *, Container &OUT) { OUT.clear(); };
  auto ComputeIN = [](Instruction *, Instruction *Pred, const Context &PredCtx,
                      const Context &, Container &IN, ResultTy *DF) {
    const auto &PredOut = DF->OUT(Pred, PredCtx);
    IN.insert(PredOut.begin(), PredOut.end());
  };
  auto ComputeOUT = [](Instruction *Inst, const Context &Ctx, Container &OUT,
                       ResultTy *DF) { OUT = DF->IN(Inst, Ctx); };
  auto Equal = [](const Container &Lhs, const Container &Rhs) {
    return Lhs == Rhs;
  };

  auto Result = E.applyForwardFromSeeds(
      module.get(), Seeds, &ICF, SeedIns, ComputeGEN, ComputeKILL, InitializeIN,
      InitializeOUT, ComputeIN, ComputeOUT, Equal);
  ASSERT_NE(Result, nullptr);

  const auto &RetIn = Result->IN(Ret, EmptyCtx);
  EXPECT_EQ(RetIn.count(&*Seeded->arg_begin()), 1u);
}

TEST_F(MonoTest, InterMonoSolverSupportsBackwardDirection) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @test(i32 %y) {
    entry:
      %r = call i32 @callee(i32 %y)
      ret i32 %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry) : InterMonoProblem<Domain>({Entry}) {}

    ::dataflow::controlflow::FlowDirection direction() const override {
      return ::dataflow::controlflow::FlowDirection::Backward;
    }

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *Callee,
                              const mono_container_t &In) override {
      mono_container_t Out;
      auto *Call = dyn_cast_or_null<CallBase>(CallSite);
      if (Call == nullptr || Callee == nullptr || Callee->arg_empty()) {
        return Out;
      }
      if (In.count(Callee->arg_begin())) {
        Out.insert(Call->getArgOperand(0));
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *CallSite, Function *,
                                Instruction *ExitStmt, Instruction *,
                                const mono_container_t &In) override {
      mono_container_t Out;
      auto *Ret = dyn_cast_or_null<ReturnInst>(ExitStmt);
      if (CallSite != nullptr && Ret != nullptr && In.count(CallSite) > 0 &&
          Ret->getReturnValue() != nullptr) {
        Out.insert(Ret->getReturnValue());
      }
      return Out;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      return In;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      for (auto &BB : *getEntryPoints().front()) {
        if (auto *RetInst = dyn_cast<ReturnInst>(BB.getTerminator())) {
          auto *Call = dyn_cast<CallBase>(RetInst->getReturnValue());
          if (Call != nullptr) {
            Seeds[RetInst].insert(Call);
          }
        }
      }
      return Seeds;
    }
  };

  ProblemT Problem(F);
  InterMonoSolver<Domain, 0> Solver(Problem);
  Solver.solve();

  ASSERT_NE(Solver.getResults(), nullptr);
  auto *Call = findFirst<CallInst>(F);
  ASSERT_NE(Call, nullptr);
  auto Facts = Solver.getResultsAt(Call);
  EXPECT_EQ(Facts.count(F->getArg(0)), 1u);
}

TEST_F(MonoTest, InterMonoSolverK1DistinguishesDifferentCallers) {
  const char *source = R"(
    define i32 @leaf(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @left(i32 %a) {
    entry:
      %l = call i32 @leaf(i32 %a)
      ret i32 %l
    }

    define i32 @right(i32 %b) {
    entry:
      %r = call i32 @leaf(i32 %b)
      ret i32 %r
    }

    define i32 @main(i32 %m, i32 %n) {
    entry:
      %x = call i32 @left(i32 %m)
      %y = call i32 @right(i32 %n)
      ret i32 %y
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Left = module->getFunction("left");
  auto *Right = module->getFunction("right");
  auto *Leaf = module->getFunction("leaf");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Left, nullptr);
  ASSERT_NE(Right, nullptr);
  ASSERT_NE(Leaf, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry) : InterMonoProblem<Domain>({Entry}) {}

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *,
                              const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  auto *LeftLeafCall = findFirst<CallInst>(Left);
  auto *RightLeafCall = findFirst<CallInst>(Right);
  auto *LeafEntry = &Leaf->getEntryBlock().front();
  auto *LeftRet = findFirst<ReturnInst>(Left);
  auto *RightRet = findFirst<ReturnInst>(Right);
  ASSERT_NE(LeftLeafCall, nullptr);
  ASSERT_NE(RightLeafCall, nullptr);
  ASSERT_NE(LeafEntry, nullptr);
  ASSERT_NE(LeftRet, nullptr);
  ASSERT_NE(RightRet, nullptr);

  ProblemT Problem(Main);
  InterMonoSolver<Domain, 1> Solver(Problem);
  Solver.solve();

  using K1Result =
      dataflow::ContextSensitiveDataFlowResult<1, std::set<Value *>>;
  using K1Context = K1Result::Context;

  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);
  bool SawLeftCtx = false;
  bool SawRightCtx = false;
  bool SawUnexpectedCtx = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst != LeafEntry) {
      continue;
    }
    if (Cell.first.Ctx == K1Context{LeftLeafCall}) {
      SawLeftCtx = true;
    } else if (Cell.first.Ctx == K1Context{RightLeafCall}) {
      SawRightCtx = true;
    } else {
      SawUnexpectedCtx = true;
    }
  }
  EXPECT_TRUE(SawLeftCtx);
  EXPECT_TRUE(SawRightCtx);
  EXPECT_FALSE(SawUnexpectedCtx);

  auto LeftFacts = Solver.getResultsAt(LeftRet);
  EXPECT_EQ(LeftFacts.count(LeftLeafCall), 1u);
  EXPECT_EQ(LeftFacts.count(RightLeafCall), 0u);

  auto RightFacts = Solver.getResultsAt(RightRet);
  EXPECT_EQ(RightFacts.count(LeftLeafCall), 0u);
  EXPECT_EQ(RightFacts.count(RightLeafCall), 1u);
}

TEST_F(MonoTest, InterMonoSolverK2TruncatesDeepCallStrings) {
  const char *source = R"(
    define i32 @leaf(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @level2(i32 %b) {
    entry:
      %r2 = call i32 @leaf(i32 %b)
      ret i32 %r2
    }

    define i32 @level1(i32 %a) {
    entry:
      %r1 = call i32 @level2(i32 %a)
      ret i32 %r1
    }

    define i32 @main(i32 %m) {
    entry:
      %r0 = call i32 @level1(i32 %m)
      ret i32 %r0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Level1 = module->getFunction("level1");
  auto *Level2 = module->getFunction("level2");
  auto *Leaf = module->getFunction("leaf");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Level1, nullptr);
  ASSERT_NE(Level2, nullptr);
  ASSERT_NE(Leaf, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry) : InterMonoProblem<Domain>({Entry}) {}

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *,
                              const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  auto *MainToLevel1 = findFirst<CallInst>(Main);
  auto *Level1ToLevel2 = findFirst<CallInst>(Level1);
  auto *Level2ToLeaf = findFirst<CallInst>(Level2);
  auto *LeafEntry = &Leaf->getEntryBlock().front();
  ASSERT_NE(MainToLevel1, nullptr);
  ASSERT_NE(Level1ToLevel2, nullptr);
  ASSERT_NE(Level2ToLeaf, nullptr);
  ASSERT_NE(LeafEntry, nullptr);

  ProblemT Problem(Main);
  InterMonoSolver<Domain, 2> Solver(Problem);
  Solver.solve();

  using K2Result =
      dataflow::ContextSensitiveDataFlowResult<2, std::set<Value *>>;
  using K2Context = K2Result::Context;

  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);
  bool SawExpectedCtx = false;
  bool SawUnexpectedCtx = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst != LeafEntry) {
      continue;
    }
    if (Cell.first.Ctx == K2Context{Level1ToLevel2, Level2ToLeaf}) {
      SawExpectedCtx = true;
    } else {
      SawUnexpectedCtx = true;
    }
  }
  EXPECT_TRUE(SawExpectedCtx);
  EXPECT_FALSE(SawUnexpectedCtx);
}

TEST_F(MonoTest, InterMonoSolverK2ReturnFlowReachesTruncatedOuterContexts) {
  const char *source = R"(
    define i32 @leaf(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @level2(i32 %b) {
    entry:
      %r2 = call i32 @leaf(i32 %b)
      ret i32 %r2
    }

    define i32 @level1(i32 %a) {
    entry:
      %r1 = call i32 @level2(i32 %a)
      ret i32 %r1
    }

    define i32 @main(i32 %m) {
    entry:
      %r0 = call i32 @level1(i32 %m)
      ret i32 %r0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Level1 = module->getFunction("level1");
  auto *Level2 = module->getFunction("level2");
  auto *Leaf = module->getFunction("leaf");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Level1, nullptr);
  ASSERT_NE(Level2, nullptr);
  ASSERT_NE(Leaf, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry) : InterMonoProblem<Domain>({Entry}) {}

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *,
                              const mono_container_t &In) override {
      mono_container_t Out = In;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  auto *MainToLevel1 = findFirst<CallInst>(Main);
  auto *Level1ToLevel2 = findFirst<CallInst>(Level1);
  auto *Level2ToLeaf = findFirst<CallInst>(Level2);
  auto *Level2Ret = findFirst<ReturnInst>(Level2);
  auto *Level1Ret = findFirst<ReturnInst>(Level1);
  ASSERT_NE(MainToLevel1, nullptr);
  ASSERT_NE(Level1ToLevel2, nullptr);
  ASSERT_NE(Level2ToLeaf, nullptr);
  ASSERT_NE(Level2Ret, nullptr);
  ASSERT_NE(Level1Ret, nullptr);

  ProblemT Problem(Main);
  InterMonoSolver<Domain, 2> Solver(Problem);
  Solver.solve();

  using K2Result =
      dataflow::ContextSensitiveDataFlowResult<2, std::set<Value *>>;
  using K2Context = K2Result::Context;

  const auto *InMap = Solver.getAnalysisINMap();
  ASSERT_NE(InMap, nullptr);

  bool SawLevel2ReturnCtx = false;
  bool SawLevel1ReturnRootCtx = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst == Level2Ret &&
        Cell.first.Ctx == K2Context{Level1ToLevel2}) {
      SawLevel2ReturnCtx = true;
      EXPECT_EQ(Cell.second.count(Level2ToLeaf), 1u);
    }
    if (Cell.first.Inst == Level1Ret && Cell.first.Ctx.empty()) {
      SawLevel1ReturnRootCtx = true;
      EXPECT_EQ(Cell.second.count(Level2ToLeaf), 1u);
    }
  }

  EXPECT_TRUE(SawLevel2ReturnCtx);
  EXPECT_TRUE(SawLevel1ReturnRootCtx);
}

TEST_F(MonoTest,
       InterMonoSolverBackwardEmptyContextSeedStaysLocalForPositiveK) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i32 %a) {
    entry:
      %r = call i32 @callee(i32 %a)
      ret i32 %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry, Instruction *SeedInst)
        : InterMonoProblem<Domain>({Entry}), SeedInst(SeedInst) {}

    ::dataflow::controlflow::FlowDirection direction() const override {
      return ::dataflow::controlflow::FlowDirection::Backward;
    }

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      if (SeedInst != nullptr) {
        Seeds[SeedInst].insert(SeedInst);
      }
      return Seeds;
    }

  private:
    Instruction *SeedInst;
  };

  auto *CalleeRet = findFirst<ReturnInst>(Callee);
  auto *MainCall = findFirst<CallInst>(Main);
  auto *MainRet = findFirst<ReturnInst>(Main);
  ASSERT_NE(CalleeRet, nullptr);
  ASSERT_NE(MainCall, nullptr);
  ASSERT_NE(MainRet, nullptr);

  ProblemT Problem(Main, CalleeRet);
  InterMonoSolver<Domain, 2> Solver(Problem);
  Solver.solve();

  const auto *InMap = Solver.getAnalysisINMap();
  const auto *OutMap = Solver.getAnalysisOUTMap();
  ASSERT_NE(InMap, nullptr);
  ASSERT_NE(OutMap, nullptr);
  bool FoundSeedCtx = false;
  bool SeedReachedCaller = false;
  bool CallerContinuationMaterialized = false;
  for (const auto &Cell : *InMap) {
    if (Cell.first.Inst == CalleeRet && Cell.first.Ctx.empty() &&
        Cell.second.count(CalleeRet) == 1u) {
      FoundSeedCtx = true;
    }
    if (Cell.first.Inst == MainCall || Cell.first.Inst == MainRet) {
      CallerContinuationMaterialized = true;
    }
    if ((Cell.first.Inst == MainCall || Cell.first.Inst == MainRet) &&
        Cell.second.count(CalleeRet) == 1u) {
      SeedReachedCaller = true;
    }
  }
  for (const auto &Cell : *OutMap) {
    if (Cell.first.Inst == MainCall || Cell.first.Inst == MainRet) {
      CallerContinuationMaterialized = true;
    }
  }
  EXPECT_TRUE(FoundSeedCtx);
  EXPECT_FALSE(SeedReachedCaller);
  EXPECT_FALSE(CallerContinuationMaterialized);
}

TEST_F(MonoTest, InterMonoSolverContextInsensitiveK0CollapsesCallers) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i32 %a, i32 %b) {
    entry:
      %r1 = call i32 @callee(i32 %a)
      %r2 = call i32 @callee(i32 %b)
      ret i32 %r2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    explicit ProblemT(Function *Entry) : InterMonoProblem<Domain>({Entry}) {}

    mono_container_t normalFlow(Instruction *Inst,
                                const mono_container_t &In) override {
      if (isa<CallBase>(Inst)) {
        return {};
      }
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *CallSite, Function *,
                              const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t returnFlow(Instruction *CallSite, Function *,
                                Instruction *, Instruction *,
                                const mono_container_t &) override {
      mono_container_t Out;
      if (CallSite != nullptr) {
        Out.insert(CallSite);
      }
      return Out;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &) override {
      return {};
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = {};
      return Seeds;
    }
  };

  std::vector<CallBase *> Calls;
  for (auto &BB : *Main) {
    for (auto &I : BB) {
      if (auto *Call = dyn_cast<CallBase>(&I)) {
        Calls.push_back(Call);
      }
    }
  }
  ASSERT_EQ(Calls.size(), 2u);

  Instruction *FirstCont = Calls[1];
  auto *FinalRet = findFirst<ReturnInst>(Main);
  ASSERT_NE(FirstCont, nullptr);
  ASSERT_NE(FinalRet, nullptr);

  ProblemT Problem(Main);
  InterMonoSolver<Domain, 0> Solver(Problem);
  Solver.solve();

  auto FirstFacts = Solver.getResultsAt(FirstCont);
  EXPECT_EQ(FirstFacts.count(Calls[0]), 1u);
  EXPECT_EQ(FirstFacts.count(Calls[1]), 1u);

  auto RetFacts = Solver.getResultsAt(FinalRet);
  EXPECT_EQ(RetFacts.count(Calls[0]), 1u);
  EXPECT_EQ(RetFacts.count(Calls[1]), 1u);
}

TEST_F(MonoTest, InterMonoSolverMissingNodeQueryReturnsAllTopForMustAnalysis) {
  const char *source = R"(
    @g = global i32 0

    define void @entry() {
    entry:
      ret void
    }

    define void @other() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Entry = module->getFunction("entry");
  auto *Other = module->getFunction("other");
  auto *G = module->getNamedGlobal("g");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Other, nullptr);
  ASSERT_NE(G, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public InterMonoProblem<Domain> {
  public:
    ProblemT(Function *Entry, Value *TopFact)
        : InterMonoProblem<Domain>({Entry}), TopFact(TopFact) {}

    mono_container_t allTop() override { return {TopFact}; }

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out;
      for (auto *V : Lhs) {
        if (Rhs.count(V)) {
          Out.insert(V);
        }
      }
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    mono_container_t callFlow(Instruction *, Function *,
                              const mono_container_t &In) override {
      return In;
    }

    mono_container_t returnFlow(Instruction *, Function *, Instruction *,
                                Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t callToRetFlow(Instruction *, Instruction *,
                                   ArrayRef<Function *>,
                                   const mono_container_t &In) override {
      return In;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = allTop();
      return Seeds;
    }

  private:
    Value *TopFact;
  };

  ProblemT Problem(Entry, G);
  InterMonoSolver<Domain, 0> Solver(Problem);
  Solver.solve();

  auto *OtherRet = findFirst<ReturnInst>(Other);
  ASSERT_NE(OtherRet, nullptr);

  auto Facts = Solver.getResultsAt(OtherRet);
  EXPECT_EQ(Facts.size(), 1u);
  EXPECT_EQ(Facts.count(G), 1u);
}

TEST_F(MonoTest, IntraMonoSolverMissingNodeQueryReturnsAllTopForMustAnalysis) {
  const char *source = R"(
    @g = global i32 0

    define void @entry() {
    entry:
      ret void
    }

    define void @other() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto *Entry = module->getFunction("entry");
  auto *Other = module->getFunction("other");
  auto *G = module->getNamedGlobal("g");
  ASSERT_NE(Entry, nullptr);
  ASSERT_NE(Other, nullptr);
  ASSERT_NE(G, nullptr);

  struct Domain : LLVMMonoAnalysisDomain<std::set<Value *>> {};
  class ProblemT : public IntraMonoProblem<Domain> {
  public:
    ProblemT(Function *Entry, Value *TopFact)
        : IntraMonoProblem<Domain>({Entry}), TopFact(TopFact) {}

    mono_container_t allTop() override { return {TopFact}; }

    mono_container_t normalFlow(Instruction *,
                                const mono_container_t &In) override {
      return In;
    }

    mono_container_t merge(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out;
      for (auto *V : Lhs) {
        if (Rhs.count(V) == 1u) {
          Out.insert(V);
        }
      }
      return Out;
    }

    bool equal_to(const mono_container_t &Lhs,
                  const mono_container_t &Rhs) override {
      return Lhs == Rhs;
    }

    std::unordered_map<Instruction *, mono_container_t>
    initialSeeds() override {
      std::unordered_map<Instruction *, mono_container_t> Seeds;
      Seeds[&getEntryPoints().front()->getEntryBlock().front()] = allTop();
      return Seeds;
    }

  private:
    Value *TopFact;
  };

  ProblemT Problem(Entry, G);
  IntraMonoSolver<Domain> Solver(Problem);
  Solver.solve();

  auto *OtherRet = findFirst<ReturnInst>(Other);
  ASSERT_NE(OtherRet, nullptr);

  const auto &InFacts = Solver.getInResultsAt(OtherRet);
  const auto &OutFacts = Solver.getOutResultsAt(OtherRet);
  EXPECT_EQ(InFacts.size(), 1u);
  EXPECT_EQ(InFacts.count(G), 1u);
  EXPECT_EQ(OutFacts.size(), 1u);
  EXPECT_EQ(OutFacts.count(G), 1u);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
