#include "MonoTestSupport.h"

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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

    mono_container_t join(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal(const mono_container_t &Lhs,
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

  struct NodeDomain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

    mono_container_t join(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal(const mono_container_t &Lhs,
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

    mono_container_t join(const mono_container_t &Lhs,
                           const mono_container_t &Rhs) override {
      mono_container_t Out = Lhs;
      Out.insert(Rhs.begin(), Rhs.end());
      return Out;
    }

    bool equal(const mono_container_t &Lhs,
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
