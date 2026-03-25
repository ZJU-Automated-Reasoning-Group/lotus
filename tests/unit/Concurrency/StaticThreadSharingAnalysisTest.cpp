#include "Analysis/Concurrency/Memory/StaticThreadSharingAnalysis.h"

#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/InitializePasses.hh"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/LegacyPassManager.h>

using namespace llvm;
using namespace lotus;
using namespace lotus::unittest;

class StaticSharingProbePass : public ModulePass {
public:
  enum class QueryKind { Instruction, Value };

  static char ID;

  StaticSharingProbePass(
      std::string functionName, std::string symbolName, QueryKind queryKind,
      StaticThreadSharingAnalysis::SharingClassification *result)
      : ModulePass(ID), m_function_name(std::move(functionName)),
        m_symbol_name(std::move(symbolName)), m_query_kind(queryKind),
        m_result(result) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<StaticThreadSharingAnalysis>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    if (!m_result) {
      return false;
    }

    const Function *F = M.getFunction(m_function_name);
    if (!F) {
      return false;
    }

    auto &sharing = getAnalysis<StaticThreadSharingAnalysis>();
    if (m_query_kind == QueryKind::Instruction) {
      const Instruction *target = findInstructionByName(*F, m_symbol_name);
      if (!target) {
        return false;
      }
      *m_result = sharing.classify(target);
    } else {
      if (const Instruction *target = findInstructionByName(*F, m_symbol_name)) {
        *m_result = sharing.classify(static_cast<const Value *>(target));
        return false;
      }
      if (const GlobalVariable *global = M.getNamedGlobal(m_symbol_name)) {
        *m_result = sharing.classify(static_cast<const Value *>(global));
        return false;
      }
      for (const Argument &arg : F->args()) {
        if (arg.getName() == m_symbol_name) {
          *m_result = sharing.classify(static_cast<const Value *>(&arg));
          return false;
        }
      }
      return false;
    }
    return false;
  }

private:
  std::string m_function_name;
  std::string m_symbol_name;
  QueryKind m_query_kind;
  StaticThreadSharingAnalysis::SharingClassification *m_result;
};

char StaticSharingProbePass::ID = 0;

class StaticSharingBoolProbePass : public ModulePass {
public:
  enum class QueryKind { Instruction, Value };

  static char ID;

  StaticSharingBoolProbePass(std::string functionName, std::string symbolName,
                             QueryKind queryKind, bool *result)
      : ModulePass(ID), m_function_name(std::move(functionName)),
        m_symbol_name(std::move(symbolName)), m_query_kind(queryKind),
        m_result(result) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<StaticThreadSharingAnalysis>();
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override {
    if (!m_result) {
      return false;
    }

    const Function *F = M.getFunction(m_function_name);
    if (!F) {
      return false;
    }

    auto &sharing = getAnalysis<StaticThreadSharingAnalysis>();
    if (m_query_kind == QueryKind::Instruction) {
      const Instruction *target = findInstructionByName(*F, m_symbol_name);
      if (!target) {
        return false;
      }
      *m_result = sharing.isShared(target);
    } else {
      if (const Instruction *target = findInstructionByName(*F, m_symbol_name)) {
        *m_result = sharing.isShared(static_cast<const Value *>(target));
        return false;
      }
      if (const GlobalVariable *global = M.getNamedGlobal(m_symbol_name)) {
        *m_result = sharing.isShared(static_cast<const Value *>(global));
        return false;
      }
      for (const Argument &arg : F->args()) {
        if (arg.getName() == m_symbol_name) {
          *m_result = sharing.isShared(static_cast<const Value *>(&arg));
          return false;
        }
      }
      return false;
    }
    return false;
  }

private:
  std::string m_function_name;
  std::string m_symbol_name;
  QueryKind m_query_kind;
  bool *m_result;
};

char StaticSharingBoolProbePass::ID = 0;

class StaticThreadSharingAnalysisTest : public lotus::unittest::LlvmModuleTest {
protected:
  static void ensurePassesInitialized() {
    static bool initialized = false;
    if (initialized) {
      return;
    }
    PassRegistry &registry = *PassRegistry::getPassRegistry();
    seadsa::initializeAnalysisPasses(registry);
    llvm::initializeDsaAnalysisPass(registry);
    initialized = true;
  }
};

TEST_F(StaticThreadSharingAnalysisTest,
       ClassifyInstructionHandlesGlobalOnlyNodes) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 1, i32* @g, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       SingleSpawnWorkerWriteDoesNotForceMultiRunSharing) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %local_slot = alloca i32, align 4
      store i32 7, i32* %local_slot, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "local_slot",
                                    StaticSharingProbePass::QueryKind::Value,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed, StaticThreadSharingAnalysis::SharingClassification::
                          DefinitelyThreadLocal);
}

TEST_F(StaticThreadSharingAnalysisTest,
       MultiSpawnWorkerLocalAllocaRemainsThreadLocal) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %local_slot = alloca i32, align 4
      store i32 7, i32* %local_slot, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null,
                               i8* (i8*)* @worker, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "local_slot",
                                    StaticSharingProbePass::QueryKind::Value,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed, StaticThreadSharingAnalysis::SharingClassification::
                          DefinitelyThreadLocal);
}

TEST_F(StaticThreadSharingAnalysisTest,
       LoopSpawnedWorkerWriteIsNotClassifiedThreadLocal) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 1, i32* @g, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker,
                               i8* null)
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "worker", "g", StaticSharingProbePass::QueryKind::Value, &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       MultiSpawnWorkerGlobalAccessStillClassifiesShared) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %load = load i32, i32* @g, align 4
      store i32 %load, i32* @g, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      call i32 @pthread_create(i8* %tid1, i8* null,
                               i8* (i8*)* @worker, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "load",
                                    StaticSharingProbePass::QueryKind::Instruction,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       AtomicWriteIsTreatedAsSharedAccess) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %rmw = atomicrmw add i32* @g, i32 1 monotonic
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "worker", "rmw", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       CompareExchangeIsTreatedAsSharedWrite) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %cas = cmpxchg i32* @g, i32 0, i32 1 monotonic monotonic
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::MaybeShared;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "worker", "cas", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(
      observed,
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       UnknownThreadEntryLeavesClassificationConservative) {
  const char *source = R"(
    @g = global i32 0, align 4
    @fp = external global i8*

    declare i32 @pthread_create(i8*, i8*, i8*, i8*)

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      %start = load i8*, i8** @fp
      call i32 @pthread_create(i8* %tid, i8* null, i8* %start, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::MaybeShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       UnknownThreadEntryMakesBooleanSharedPredicateConservative) {
  const char *source = R"(
    @g = global i32 0, align 4
    @fp = external global i8*

    declare i32 @pthread_create(i8*, i8*, i8*, i8*)

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      %start = load i8*, i8** @fp
      call i32 @pthread_create(i8* %tid, i8* null, i8* %start, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  bool observed = false;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingBoolProbePass(
      "main", "main_load", StaticSharingBoolProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_TRUE(observed);
}

TEST_F(StaticThreadSharingAnalysisTest,
       UnknownMemoryIdentityStaysConservativeAtInstructionLevel) {
  const char *source = R"(
    @unknown_ptr = external global i32*

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %ptr = load i32*, i32** @unknown_ptr, align 8
      %val = load i32, i32* %ptr, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8, align 1
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "val",
                                    StaticSharingProbePass::QueryKind::Instruction,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::MaybeShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       MultiRunDistinctHeapPayloadsFollowPaperMultiRunRule) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %typed = bitcast i8* %arg to i32*
      store i32 1, i32* %typed, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8, align 1
      %tid2 = alloca i8, align 1
      %alloc1 = call i8* @malloc(i64 4)
      %alloc2 = call i8* @malloc(i64 4)
      call i32 @pthread_create(i8* %tid1, i8* null,
                               i8* (i8*)* @worker, i8* %alloc1)
      call i32 @pthread_create(i8* %tid2, i8* null,
                               i8* (i8*)* @worker, i8* %alloc2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "arg",
                                    StaticSharingProbePass::QueryKind::Value,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       ConstantIndexedArrayAccessesCollapseToSingleSharedArrayField) {
  const char *source = R"(
    @arr = global [2 x i32] zeroinitializer, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %slot1 = getelementptr inbounds [2 x i32], [2 x i32]* @arr, i64 0, i64 1
      store i32 1, i32* %slot1, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %slot0 = getelementptr inbounds [2 x i32], [2 x i32]* @arr, i64 0, i64 0
      %main_load = load i32, i32* %slot0, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       MemsetWritesParticipateInSharingClassification) {
  const char *source = R"(
    @g = global [4 x i8] zeroinitializer, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)

    define i8* @worker(i8* %arg) {
    entry:
      %dst = bitcast [4 x i8]* @g to i8*
      call void @llvm.memset.p0i8.i64(i8* %dst, i8 7, i64 4, i1 false)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %slot0 = getelementptr inbounds [4 x i8], [4 x i8]* @g, i64 0, i64 0
      %main_load = load i8, i8* %slot0, align 1
      %ext = zext i8 %main_load to i32
      ret i32 %ext
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       UnresolvedIndirectWorkerCallDoesNotClaimThreadLocal) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define void @target() {
    entry:
      store i32 1, i32* @g, align 4
      ret void
    }

    define i8* @worker(i8* %arg) {
    entry:
      %fp = select i1 true, void ()* @target, void ()* @target
      call void %fp()
      ret i8* null
    }

    define i32 @main() {
    entry:
      %spawn = call i32 @pthread_create(i8* null, i8* null,
                                        i8* (i8*)* @worker, i8* null)
      %main_load = load i32, i32* @g, align 4
      ret i32 %main_load
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass(
      "main", "main_load", StaticSharingProbePass::QueryKind::Instruction,
      &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::MaybeShared);
}

TEST_F(StaticThreadSharingAnalysisTest,
       HelperMediatedRepeatedSpawnMarksWorkerOnlyGlobalAsShared) {
  const char *source = R"(
    @g = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      store i32 1, i32* @g, align 4
      ret i8* null
    }

    define void @spawn_once() {
    entry:
      %tid = alloca i8, align 1
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      call void @spawn_once()
      call void @spawn_once()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ensurePassesInitialized();
  ThreadAPI::resetThreadAPI();
  StaticThreadSharingAnalysis::SharingClassification observed =
      StaticThreadSharingAnalysis::SharingClassification::DefinitelyThreadLocal;

  legacy::PassManager PM;
  PM.add(new seadsa::DsaAnalysis());
  PM.add(new StaticThreadSharingAnalysis());
  PM.add(new StaticSharingProbePass("worker", "g",
                                    StaticSharingProbePass::QueryKind::Value,
                                    &observed));
  PM.run(*module);

  EXPECT_EQ(observed,
            StaticThreadSharingAnalysis::SharingClassification::DefinitelyShared);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
