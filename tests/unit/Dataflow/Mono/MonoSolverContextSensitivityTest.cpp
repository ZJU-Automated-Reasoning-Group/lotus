#include "MonoTestSupport.h"

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

  struct TrivialDomain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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
