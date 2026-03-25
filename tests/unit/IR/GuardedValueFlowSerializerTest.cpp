#include "IR/GVFG/GuardedValueFlowGraph.h"
#include "IR/GVFG/GuardedValueFlowSerializer.h"
#include "IR/GVFG/LotusAdapter.h"
#include "TestUtils/LLVMHelpers.h"

#include <fstream>
#include <string>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace llvm;
using namespace lotus::gvfg;
using namespace lotus::unittest;

namespace {

class GuardedValueFlowSerializerTest : public LlvmModuleTest {
protected:
  struct Pipeline {
    std::unique_ptr<legacy::PassManager> pm;
    LotusAA *lotus{nullptr};
    GuardedValueFlowGraphBuilderPass *builder{nullptr};
  };

  static void initializePassInfra() {
    static bool initialized = false;
    if (initialized)
      return;

    auto &registry = *PassRegistry::getPassRegistry();
    initializeCore(registry);
    initializeAnalysis(registry);
    initializeTransformUtils(registry);
    initialized = true;
  }

  Pipeline runAdapter(Module &M) {
    initializePassInfra();
    Pipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.lotus = new LotusAA();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.lotus);
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->add(new LotusGuardedValueFlowAdapterPass());
    pipeline.pm->run(M);
    return pipeline;
  }

  Pipeline runBuilder(Module &M) {
    initializePassInfra();
    Pipeline pipeline;
    pipeline.pm = std::make_unique<legacy::PassManager>();
    pipeline.builder = new GuardedValueFlowGraphBuilderPass();
    pipeline.pm->add(new gsa::ControlDependenceAnalysisPass());
    pipeline.pm->add(new gsa::GateAnalysisPass());
    pipeline.pm->add(pipeline.builder);
    pipeline.pm->run(M);
    return pipeline;
  }
};

TEST_F(GuardedValueFlowSerializerTest, EmitsTextAndDotForAdaptedGraph) {
  const char *source = R"(
    define i32 @test(i32* %p, i32 %v) {
    entry:
      store i32 %v, i32* %p
      %loaded = load i32, i32* %p
      ret i32 %loaded
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runAdapter(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);

  std::string text = GuardedValueFlowSerializer::toText(graph);
  EXPECT_NE(text.find("GVFG-TEXT-V1"), std::string::npos);
  EXPECT_NE(text.find("function \"test\""), std::string::npos);
  EXPECT_NE(text.find("nodes "), std::string::npos);
  EXPECT_NE(text.find("edges "), std::string::npos);
  EXPECT_NE(text.find("sites "), std::string::npos);
  EXPECT_NE(text.find("LoadMemory"), std::string::npos);

  std::string dot = GuardedValueFlowSerializer::toDot(graph);
  EXPECT_NE(dot.find("digraph \"gvfg.test\""), std::string::npos);
  EXPECT_NE(dot.find("rankdir=LR"), std::string::npos);
  EXPECT_NE(dot.find("uses"), std::string::npos);
  EXPECT_NE(dot.find("box3d"), std::string::npos);

  SmallString<256> text_path;
  SmallString<256> dot_path;
  int text_fd = -1;
  int dot_fd = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("gvfg-serializer", "txt", text_fd,
                                            text_path));
  ASSERT_FALSE(
      sys::fs::createTemporaryFile("gvfg-serializer", "dot", dot_fd, dot_path));
  ::close(text_fd);
  ::close(dot_fd);

  ASSERT_TRUE(GuardedValueFlowSerializer::writeText(graph, text_path.str().str()));
  ASSERT_TRUE(GuardedValueFlowSerializer::writeDot(graph, dot_path.str().str()));

  std::ifstream text_in(text_path.str().str());
  std::ifstream dot_in(dot_path.str().str());
  ASSERT_TRUE(text_in.good());
  ASSERT_TRUE(dot_in.good());
  sys::fs::remove(text_path);
  sys::fs::remove(dot_path);
}

TEST_F(GuardedValueFlowSerializerTest, PersistsDiagnosticsAndUnknownNodes) {
  const char *source = R"(
    define i32 @test({i32, i32} %pair) {
    entry:
      %field = extractvalue {i32, i32} %pair, 0
      ret i32 %field
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto pipeline = runBuilder(*module);
  ASSERT_TRUE(pipeline.builder->hasGraphFor(*F));
  GuardedValueFlowGraph &graph = pipeline.builder->getGraph(*F);
  ASSERT_TRUE(graph.hasDiagnostics());

  std::string text = GuardedValueFlowSerializer::toText(graph);
  EXPECT_NE(text.find("diagnostic"), std::string::npos);
  EXPECT_NE(text.find("Unknown"), std::string::npos);
  EXPECT_NE(text.find("extractvalue"), std::string::npos);

  std::string dot = GuardedValueFlowSerializer::toDot(graph);
  EXPECT_NE(dot.find("d0"), std::string::npos);
  EXPECT_NE(dot.find("octagon"), std::string::npos);
}

} // namespace
