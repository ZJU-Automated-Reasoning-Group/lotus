#include "MonoTestSupport.h"

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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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

  struct Domain : LLVMMonoAnalysisTypes<std::set<Value *>> {};
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
