#include "Analysis/ControlDependence/ControlDependence.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "Analysis/ControlDependence/CompactControlDependence.h"
#include "Analysis/ControlDependence/DOD.h"
#include "Analysis/ControlDependence/ICFGControlDependence.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

using lotus::cd::Algorithm;
using lotus::cd::ControlDependenceAnalysis;
using lotus::cd::ControlDependenceOptions;
using lotus::cd::ICFGControlDependenceAnalysis;

namespace {

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &context,
                                          llvm::StringRef ir) {
  llvm::SMDiagnostic error;
  auto module = llvm::parseAssemblyString(ir, error, context);
  if (!module)
    error.print("ControlDependenceTest", llvm::errs());
  return module;
}

llvm::BasicBlock *block(llvm::Function &function, llvm::StringRef name) {
  for (llvm::BasicBlock &candidate : function)
    if (candidate.getName() == name)
      return &candidate;
  return nullptr;
}

ControlDependenceAnalysis analyze(llvm::Function &function,
                                  Algorithm algorithm) {
  return ControlDependenceAnalysis(function,
                                   ControlDependenceOptions{algorithm});
}

using Triple = std::tuple<unsigned, unsigned, unsigned>;

bool reachableBefore(lotus::cd::detail::GraphNode *start,
                     lotus::cd::detail::GraphNode *target,
                     lotus::cd::detail::GraphNode *forbidden,
                     size_t nodeCount) {
  if (start == forbidden)
    return false;
  if (start == target)
    return true;
  std::vector<bool> seen(nodeCount + 1, false);
  std::vector<lotus::cd::detail::GraphNode *> worklist{start};
  seen[start->getID()] = true;
  while (!worklist.empty()) {
    auto *node = worklist.back();
    worklist.pop_back();
    for (auto *successor : node->successors()) {
      if (successor == forbidden)
        continue;
      if (successor == target)
        return true;
      if (!seen[successor->getID()]) {
        seen[successor->getID()] = true;
        worklist.push_back(successor);
      }
    }
  }
  return false;
}

bool bruteInevitable(lotus::cd::detail::Graph &graph,
                     lotus::cd::detail::GraphNode *source,
                     lotus::cd::detail::GraphNode *target) {
  if (source == target)
    return true;

  std::vector<bool> reachable(graph.size() + 1, false);
  std::vector<lotus::cd::detail::GraphNode *> worklist{source};
  reachable[source->getID()] = true;
  while (!worklist.empty()) {
    auto *node = worklist.back();
    worklist.pop_back();
    if (node->successors().empty())
      return false; // A finite maximal path avoids target.
    for (auto *successor : node->successors()) {
      if (successor == target || reachable[successor->getID()])
        continue;
      reachable[successor->getID()] = true;
      worklist.push_back(successor);
    }
  }

  // A reachable cycle in G-{target} gives an infinite maximal path avoiding
  // target. Absence of both such a cycle and a reachable sink forces target.
  std::vector<unsigned char> color(graph.size() + 1, 0);
  std::function<bool(lotus::cd::detail::GraphNode *)> hasCycle =
      [&](auto *node) {
        color[node->getID()] = 1;
        for (auto *successor : node->successors()) {
          if (successor == target || !reachable[successor->getID()])
            continue;
          if (color[successor->getID()] == 1)
            return true;
          if (color[successor->getID()] == 0 && hasCycle(successor))
            return true;
        }
        color[node->getID()] = 2;
        return false;
      };
  for (auto *node : graph.nodes())
    if (reachable[node->getID()] && color[node->getID()] == 0 && hasCycle(node))
      return false;
  return true;
}

std::set<Triple>
bruteDOD(lotus::cd::detail::Graph &graph,
         const lotus::cd::detail::Inevitability &inevitability) {
  std::set<Triple> result;
  for (auto *decision : graph.predicates()) {
    if (decision->successors().size() != 2)
      continue;
    auto *firstSuccessor = decision->successors()[0];
    auto *secondSuccessor = decision->successors()[1];
    for (unsigned firstID = 1; firstID <= graph.size(); ++firstID) {
      auto *first = graph.getNode(firstID);
      if (first == decision || !inevitability.contains(decision, first))
        continue;
      for (unsigned secondID = firstID + 1; secondID <= graph.size();
           ++secondID) {
        auto *second = graph.getNode(secondID);
        if (second == decision || !inevitability.contains(decision, second))
          continue;
        bool firstBeforeSecond1 =
            !reachableBefore(firstSuccessor, second, first, graph.size());
        bool secondBeforeFirst1 =
            !reachableBefore(firstSuccessor, first, second, graph.size());
        bool firstBeforeSecond2 =
            !reachableBefore(secondSuccessor, second, first, graph.size());
        bool secondBeforeFirst2 =
            !reachableBefore(secondSuccessor, first, second, graph.size());
        if ((firstBeforeSecond1 && secondBeforeFirst2) ||
            (secondBeforeFirst1 && firstBeforeSecond2))
          result.insert({decision->getID(), firstID, secondID});
      }
    }
  }
  return result;
}

std::set<Triple>
enumerateCompactDOD(const lotus::cd::detail::Graph &graph,
                    const lotus::cd::detail::DODBicliqueMap &bicliques) {
  std::set<Triple> result;
  lotus::cd::detail::forEachDODPair(
      graph, bicliques, [&](auto *decision, auto *first, auto *second) {
        result.insert({decision->getID(), first->getID(), second->getID()});
      });
  return result;
}

constexpr llvm::StringLiteral DiamondIR = R"(
  define void @diamond(i1 %condition) {
  entry:
    br i1 %condition, label %left, label %right
  left:
    br label %exit
  right:
    br label %exit
  exit:
    ret void
  }
)";

constexpr llvm::StringLiteral NonterminatingChoiceIR = R"(
  define void @nonterminating_choice(i1 %condition) {
  entry:
    br i1 %condition, label %left, label %right
  left:
    br label %left
  right:
    br label %right
  }
)";

constexpr llvm::StringLiteral DecisiveOrderIR = R"(
  define void @decisive_order(i1 %condition) {
  entry:
    br i1 %condition, label %blue, label %red
  blue:
    br label %blue_body
  blue_body:
    br label %red
  red:
    br label %red_body
  red_body:
    br label %blue
  }
)";

TEST(ControlDependenceTest, StandardComputesDiamondDependencesAndInverse) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DiamondIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("diamond");
  auto analysis = analyze(function, Algorithm::Standard);

  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");
  llvm::BasicBlock *exit = block(function, "exit");
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(left, nullptr);
  ASSERT_NE(right, nullptr);
  ASSERT_NE(exit, nullptr);

  EXPECT_TRUE(analysis.dependsOn(left, entry));
  EXPECT_TRUE(analysis.dependsOn(right, entry));
  EXPECT_FALSE(analysis.dependsOn(exit, entry));
  ASSERT_EQ(analysis.getDependents(entry).size(), 2u);
  EXPECT_EQ(analysis.getDependents(entry)[0], left);
  EXPECT_EQ(analysis.getDependents(entry)[1], right);
}

TEST(ControlDependenceTest, StandardMatchesDgOnLoopsAndMultipleExits) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @loop(i1 %condition) {
    entry:
      br label %header
    header:
      br i1 %condition, label %body, label %exit
    body:
      br label %header
    exit:
      ret void
    }

    define void @multiple_exits(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      ret void
    right:
      ret void
    }
  )");
  ASSERT_TRUE(module);

  llvm::Function &loop = *module->getFunction("loop");
  llvm::BasicBlock *header = block(loop, "header");
  llvm::BasicBlock *body = block(loop, "body");
  auto loopAnalysis = analyze(loop, Algorithm::Standard);
  EXPECT_TRUE(loopAnalysis.dependsOn(body, header));
  EXPECT_TRUE(loopAnalysis.dependsOn(header, header));

  llvm::Function &multipleExits = *module->getFunction("multiple_exits");
  llvm::BasicBlock *entry = block(multipleExits, "entry");
  llvm::BasicBlock *left = block(multipleExits, "left");
  llvm::BasicBlock *right = block(multipleExits, "right");
  auto exitAnalysis = analyze(multipleExits, Algorithm::Standard);
  EXPECT_TRUE(exitAnalysis.dependsOn(left, entry));
  EXPECT_TRUE(exitAnalysis.dependsOn(right, entry));
}

TEST(ControlDependenceTest, NTSCDImplementationsAgreeOnDiamond) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DiamondIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("diamond");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");

  for (Algorithm algorithm :
       {Algorithm::NTSCD, Algorithm::NTSCD2, Algorithm::NTSCDLegacy,
        Algorithm::NTSCDRanganath, Algorithm::NTSCDRanganathOriginal}) {
    auto analysis = analyze(function, algorithm);
    EXPECT_TRUE(analysis.dependsOn(left, entry));
    EXPECT_TRUE(analysis.dependsOn(right, entry));
  }
}

TEST(ControlDependenceTest, NTSCDHandlesFunctionsWithoutExits) {
  llvm::LLVMContext context;
  auto module = parseModule(context, NonterminatingChoiceIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("nonterminating_choice");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *left = block(function, "left");
  llvm::BasicBlock *right = block(function, "right");

  auto ntscd = analyze(function, Algorithm::NTSCD);
  auto ntscd2 = analyze(function, Algorithm::NTSCD2);
  EXPECT_TRUE(ntscd.dependsOn(left, entry));
  EXPECT_TRUE(ntscd.dependsOn(right, entry));
  EXPECT_TRUE(ntscd2.dependsOn(left, entry));
  EXPECT_TRUE(ntscd2.dependsOn(right, entry));
}

TEST(ControlDependenceTest, NTSCD2DoesNotReenqueueColoredSelfLoopTarget) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @self_loop_target(i1 %condition) {
    entry:
      br i1 %condition, label %loop, label %entry
    loop:
      br label %loop
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("self_loop_target");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *loop = block(function, "loop");

  auto ntscd2 = analyze(function, Algorithm::NTSCD2);
  auto combined = analyze(function, Algorithm::DODNTSCD);
  EXPECT_TRUE(ntscd2.dependsOn(loop, entry));
  EXPECT_TRUE(combined.dependsOn(loop, entry));
}

TEST(ControlDependenceTest, CompactNTSCDSupportsMultiwayDecisions) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @multiway(i32 %selector) {
    entry:
      switch i32 %selector, label %avoid [
        i32 0, label %first
        i32 1, label %second
      ]
    first:
      br label %target
    second:
      br label %target
    avoid:
      br label %exit
    target:
      br label %exit
    exit:
      ret void
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("multiway");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *target = block(function, "target");
  auto analysis = analyze(function, Algorithm::NTSCDCompact);
  EXPECT_TRUE(analysis.dependsOn(target, entry));
}

TEST(ControlDependenceTest, DODFindsDecisiveOrderOnInfiniteCycle) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("decisive_order");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *blueBody = block(function, "blue_body");
  llvm::BasicBlock *redBody = block(function, "red_body");

  auto dod = analyze(function, Algorithm::DOD);
  auto ranganath = analyze(function, Algorithm::DODRanganath);
  auto combined = analyze(function, Algorithm::DODNTSCD);
  EXPECT_TRUE(dod.dependsOn(blueBody, entry));
  EXPECT_TRUE(dod.dependsOn(redBody, entry));
  EXPECT_TRUE(ranganath.dependsOn(blueBody, entry));
  EXPECT_TRUE(ranganath.dependsOn(redBody, entry));
  EXPECT_TRUE(combined.dependsOn(blueBody, entry));
  EXPECT_TRUE(combined.dependsOn(redBody, entry));
}

TEST(ControlDependenceTest, CompactDODExposesCanonicalBicliqueAndClosure) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("decisive_order");
  llvm::BasicBlock *entry = block(function, "entry");
  llvm::BasicBlock *blue = block(function, "blue");
  llvm::BasicBlock *blueBody = block(function, "blue_body");
  llvm::BasicBlock *red = block(function, "red");
  llvm::BasicBlock *redBody = block(function, "red_body");

  auto analysis = analyze(function, Algorithm::DODNTSCDCompact);
  ASSERT_TRUE(analysis.hasDODBiclique(entry));
  EXPECT_EQ(analysis.getDODLeft(entry),
            (ControlDependenceAnalysis::BlockVector{blue, blueBody}));
  EXPECT_EQ(analysis.getDODRight(entry),
            (ControlDependenceAnalysis::BlockVector{red, redBody}));
  EXPECT_TRUE(analysis.isDOD(entry, blueBody, redBody));
  EXPECT_FALSE(analysis.isDOD(entry, blue, blueBody));
  EXPECT_TRUE(analysis.dependsOn(blueBody, entry));
  EXPECT_TRUE(analysis.dependsOn(redBody, entry));

  auto closure = analysis.getDependencyClosure({blueBody, redBody});
  EXPECT_NE(std::find(closure.begin(), closure.end(), entry), closure.end());
}

TEST(ControlDependenceTest, BaselineAndCompactStreamTheSameExactDODPairs) {
  lotus::cd::detail::Graph graph;
  std::vector<lotus::cd::detail::GraphNode *> nodes;
  for (unsigned index = 0; index < 5; ++index)
    nodes.push_back(&graph.createNode());
  graph.addEdge(*nodes[0], *nodes[1]);
  graph.addEdge(*nodes[0], *nodes[3]);
  graph.addEdge(*nodes[1], *nodes[2]);
  graph.addEdge(*nodes[2], *nodes[3]);
  graph.addEdge(*nodes[3], *nodes[4]);
  graph.addEdge(*nodes[4], *nodes[1]);

  auto inevitability = lotus::cd::detail::computeInevitability(graph);
  auto bicliques = lotus::cd::detail::computeCompactDOD(graph, inevitability);
  std::set<Triple> compact = enumerateCompactDOD(graph, bicliques);
  std::set<Triple> baseline;
  size_t baselineCount = 0;
  lotus::cd::detail::forEachBaselineDODPair(
      graph, [&](auto *decision, auto *first, auto *second) {
        ++baselineCount;
        unsigned firstID = std::min(first->getID(), second->getID());
        unsigned secondID = std::max(first->getID(), second->getID());
        baseline.insert({decision->getID(), firstID, secondID});
      });
  EXPECT_EQ(baseline, compact);
  EXPECT_EQ(baselineCount, compact.size());
  EXPECT_EQ(compact.size(), 4u);
}

TEST(ControlDependenceTest, EveryBinaryAlgorithmMaintainsInverseRelation) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("decisive_order");

  for (Algorithm algorithm :
       {Algorithm::Standard, Algorithm::NTSCD, Algorithm::NTSCD2,
        Algorithm::NTSCDLegacy, Algorithm::NTSCDRanganath,
        Algorithm::NTSCDRanganathOriginal, Algorithm::DOD,
        Algorithm::DODRanganath, Algorithm::DODNTSCD, Algorithm::NTSCDCompact,
        Algorithm::DODCompact, Algorithm::DODNTSCDCompact}) {
    auto analysis = analyze(function, algorithm);
    for (llvm::BasicBlock &dependent : function)
      for (const llvm::BasicBlock *predicate :
           analysis.getDependencies(&dependent)) {
        auto inverse = analysis.getDependents(predicate);
        EXPECT_NE(std::find(inverse.begin(), inverse.end(), &dependent),
                  inverse.end());
      }
  }
}

TEST(ControlDependenceTest, StrongClosureIsStableAndFunctionOrdered) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @closure(i1 %condition) {
    b2:
      br label %b3
    b3:
      br i1 %condition, label %b0, label %b1
    b1:
      br label %b0
    b0:
      br label %b0
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("closure");
  llvm::BasicBlock *b2 = block(function, "b2");
  llvm::BasicBlock *b3 = block(function, "b3");
  llvm::BasicBlock *b1 = block(function, "b1");

  auto analysis = analyze(function, Algorithm::StrongControlClosure);
  auto closure = analysis.getClosure({b1, b2, b1});
  ASSERT_EQ(closure.size(), 3u);
  EXPECT_EQ(closure[0], b2);
  EXPECT_EQ(closure[1], b3);
  EXPECT_EQ(closure[2], b1);

  auto closedAgain = analysis.getClosure(closure);
  EXPECT_EQ(closedAgain, closure);
  EXPECT_TRUE(analysis.getDependencies(b1).empty());
}

TEST(ControlDependenceTest, StrongClosureHandlesColoredTargetCycles) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define void @closure_cycle(i1 %condition) {
    start:
      br label %predicate
    predicate:
      br i1 %condition, label %loop, label %predicate
    loop:
      br label %loop
    }
  )");
  ASSERT_TRUE(module);
  llvm::Function &function = *module->getFunction("closure_cycle");
  llvm::BasicBlock *start = block(function, "start");
  llvm::BasicBlock *predicate = block(function, "predicate");
  llvm::BasicBlock *loop = block(function, "loop");

  auto analysis = analyze(function, Algorithm::StrongControlClosure);
  auto closure = analysis.getClosure({start, loop});
  ASSERT_EQ(closure.size(), 3u);
  EXPECT_EQ(closure[0], start);
  EXPECT_EQ(closure[1], predicate);
  EXPECT_EQ(closure[2], loop);
}

TEST(ControlDependenceTest,
     CompactDODAndClosureMatchDefinitionsOnAllThreeNodeGraphs) {
  constexpr unsigned nodeCount = 3;
  constexpr unsigned graphCount = 1u << (nodeCount * nodeCount);
  for (unsigned mask = 0; mask < graphCount; ++mask) {
    lotus::cd::detail::Graph graph;
    std::vector<lotus::cd::detail::GraphNode *> nodes;
    for (unsigned index = 0; index < nodeCount; ++index)
      nodes.push_back(&graph.createNode());
    for (unsigned source = 0; source < nodeCount; ++source)
      for (unsigned target = 0; target < nodeCount; ++target)
        if (mask & (1u << (source * nodeCount + target)))
          graph.addEdge(*nodes[source], *nodes[target]);

    auto inevitability = lotus::cd::detail::computeInevitability(graph);
    for (auto *source : graph.nodes())
      for (auto *target : graph.nodes())
        EXPECT_EQ(inevitability.contains(source, target),
                  bruteInevitable(graph, source, target))
            << "graph mask " << mask << ", source " << source->getID()
            << ", target " << target->getID();
    auto ntscd = lotus::cd::detail::computeCompactNTSCD(graph, inevitability);
    auto bicliques = lotus::cd::detail::computeCompactDOD(graph, inevitability);
    std::set<Triple> expectedDOD = bruteDOD(graph, inevitability);
    std::set<Triple> compactDOD = enumerateCompactDOD(graph, bicliques);
    EXPECT_EQ(compactDOD, expectedDOD) << "graph mask " << mask;

    for (unsigned seedMask = 0; seedMask < (1u << nodeCount); ++seedMask) {
      lotus::cd::detail::NodeSet seed;
      std::set<unsigned> expected;
      for (unsigned index = 0; index < nodeCount; ++index)
        if (seedMask & (1u << index)) {
          seed.insert(nodes[index]);
          expected.insert(index + 1);
        }

      bool changed;
      do {
        changed = false;
        for (const auto &entry : ntscd.first) {
          if (!expected.count(entry.first->getID()))
            continue;
          for (auto *decision : entry.second)
            changed |= expected.insert(decision->getID()).second;
        }
        for (const Triple &triple : expectedDOD) {
          unsigned decision;
          unsigned first;
          unsigned second;
          std::tie(decision, first, second) = triple;
          if (expected.count(first) && expected.count(second))
            changed |= expected.insert(decision).second;
        }
      } while (changed);

      auto closure = lotus::cd::detail::computeCompactDependencyClosure(
          graph, seed, ntscd, bicliques);
      std::set<unsigned> actual;
      for (auto *node : closure)
        actual.insert(node->getID());
      EXPECT_EQ(actual, expected)
          << "graph mask " << mask << ", seed mask " << seedMask;
    }
  }
}

TEST(ControlDependenceTest, CompactDODMatchesDefinitionOnAllFourNodeGraphs) {
  constexpr unsigned nodeCount = 4;
  constexpr unsigned graphCount = 1u << (nodeCount * nodeCount);
  for (unsigned mask = 0; mask < graphCount; ++mask) {
    lotus::cd::detail::Graph graph;
    std::vector<lotus::cd::detail::GraphNode *> nodes;
    for (unsigned index = 0; index < nodeCount; ++index)
      nodes.push_back(&graph.createNode());
    for (unsigned source = 0; source < nodeCount; ++source)
      for (unsigned target = 0; target < nodeCount; ++target)
        if (mask & (1u << (source * nodeCount + target)))
          graph.addEdge(*nodes[source], *nodes[target]);

    auto inevitability = lotus::cd::detail::computeInevitability(graph);
    auto bicliques = lotus::cd::detail::computeCompactDOD(graph, inevitability);
    ASSERT_EQ(enumerateCompactDOD(graph, bicliques),
              bruteDOD(graph, inevitability))
        << "graph mask " << mask;
  }
}

TEST(ControlDependenceTest, ICFGAdapterRunsGraphAlgorithmsOnLotusICFG) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @main(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      br label %exit
    right:
      br label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  llvm::Function &function = *module->getFunction("main");
  ICFGNode *entry = icfg.getIntraBlockNode(block(function, "entry"));
  ICFGNode *left = icfg.getIntraBlockNode(block(function, "left"));
  ICFGNode *right = icfg.getIntraBlockNode(block(function, "right"));

  ICFGControlDependenceAnalysis analysis(
      icfg, ControlDependenceOptions{Algorithm::NTSCD2});
  EXPECT_TRUE(analysis.dependsOn(left, entry));
  EXPECT_TRUE(analysis.dependsOn(right, entry));
  ASSERT_EQ(analysis.getDependents(entry).size(), 2u);
  EXPECT_LT(analysis.getDependents(entry)[0]->getId(),
            analysis.getDependents(entry)[1]->getId());
}

TEST(ControlDependenceTest, ICFGAdapterFindsNoReturnCallDependence) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @abort() noreturn

    define void @foo(i1 %condition) {
    entry:
      br i1 %condition, label %normal, label %die
    normal:
      ret void
    die:
      call void @abort()
      unreachable
    }

    define i32 @main(i1 %condition) {
    entry:
      call void @foo(i1 %condition)
      br label %after
    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());
  llvm::Function &foo = *module->getFunction("foo");
  llvm::Function &main = *module->getFunction("main");
  ICFGNode *fooEntry = icfg.getIntraBlockNode(block(foo, "entry"));
  ICFGNode *after = icfg.getIntraBlockNode(block(main, "after"));

  ICFGControlDependenceAnalysis analysis(
      icfg, ControlDependenceOptions{Algorithm::NTSCD});
  EXPECT_TRUE(analysis.dependsOn(after, fooEntry));
}

TEST(ControlDependenceTest, ICFGAdapterExposesCompactDODBicliques) {
  llvm::LLVMContext context;
  auto module = parseModule(context, DecisiveOrderIR);
  ASSERT_TRUE(module);
  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  llvm::Function &function = *module->getFunction("decisive_order");
  ICFGNode *entry = icfg.getIntraBlockNode(block(function, "entry"));
  ICFGNode *blueBody = icfg.getIntraBlockNode(block(function, "blue_body"));
  ICFGNode *redBody = icfg.getIntraBlockNode(block(function, "red_body"));
  ICFGControlDependenceAnalysis analysis(
      icfg, ControlDependenceOptions{Algorithm::DODNTSCDCompact});

  EXPECT_TRUE(analysis.hasDODBiclique(entry));
  EXPECT_TRUE(analysis.isDOD(entry, blueBody, redBody));
  auto closure = analysis.getDependencyClosure({blueBody, redBody});
  EXPECT_NE(std::find(closure.begin(), closure.end(), entry), closure.end());
}

} // namespace
