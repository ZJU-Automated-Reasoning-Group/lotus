#pragma once

/**
 * @file EliminationTestSupport.h
 * @brief Unit tests for elimination-based (state elimination) dataflow solver
 */

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/APA/Analyses/Inter/Lockset.h"
#include "Dataflow/APA/Analyses/Inter/Reachability.h"
#include "Dataflow/APA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/APA/Analyses/Inter/UninitializedVariables.h"
#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"
#include "Dataflow/APA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/APA/Analyses/Intra/Reachability.h"
#include "TestUtils/LLVMHelpers.h"

#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using lotus::unittest::findInstructionByName;

namespace elimination_test {

class APATest : public ::testing::Test {
protected:
  llvm::LLVMContext Context;

  template <typename InstT> InstT *findFirst(llvm::Function *F) {
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *Match = llvm::dyn_cast<InstT>(&I)) {
          return Match;
        }
      }
    }
    return nullptr;
  }
};

template <typename ResultT, typename NodeT>
const typename ResultT::fact_t &factAt(const ResultT &Res, const NodeT &N) {
  auto *Fact = Res.tryIN(N);
  EXPECT_NE(Fact, nullptr);
  static const typename ResultT::fact_t Empty{};
  return Fact != nullptr ? *Fact : Empty;
}

struct TestDomain {
  using n_t = int;
  using fact_t = std::set<int>;
  using transfer_t = int; // "gen label"
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

class ReachabilityProblem final
    : public elimination::IntraEliminationProblem<TestDomain> {
public:
  explicit ReachabilityProblem(int Entry,
                               std::unordered_map<int, std::vector<int>> Succs)
      : Entry(Entry), Succs(std::move(Succs)) {}

  std::vector<int> nodes() const override {
    std::vector<int> Ns;
    Ns.reserve(Succs.size());
    for (const auto &It : Succs) {
      Ns.push_back(It.first);
    }
    return Ns;
  }

  int entry() const override { return Entry; }

  std::vector<int> succs(int Node) const override {
    auto It = Succs.find(Node);
    if (It == Succs.end()) {
      return {};
    }
    return It->second;
  }

  int edgeTransfer(int /*Src*/, int Dst) const override { return Dst; }

  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t Out = In;
    Out.insert(T);
    return Out;
  }

  fact_t join(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t bottom() const override { return {}; }

  fact_t initialFact() const override { return {}; }

  std::size_t maxStarIterations() const override { return 1000; }

private:
  int Entry;
  std::unordered_map<int, std::vector<int>> Succs;
};

} // namespace elimination_test

namespace elimination_test {

class ReducibleReachabilityProblem final
    : public elimination::IntraReducibleEliminationProblem<TestDomain> {
public:
  std::vector<int> nodes() const override { return {0, 1, 2, 3}; }

  int entry() const override { return 0; }

  std::vector<int> succs(int Node) const override {
    switch (Node) {
    case 0:
      return {1};
    case 1:
      return {2, 3};
    case 2:
      return {1};
    case 3:
    default:
      return {};
    }
  }

  std::vector<Edge> edges() const override {
    return {{0, 1}, {1, 2}, {2, 1}, {1, 3}};
  }

  std::vector<int> topologicalOrder() const override { return {0, 1, 2, 3}; }

  int idom(int Node) const override {
    switch (Node) {
    case 0:
    case 1:
      return 0;
    case 2:
    case 3:
      return 1;
    default:
      return 0;
    }
  }

  bool dominates(int A, int B) const override {
    if (A == B) {
      return true;
    }
    if (A == 0) {
      return true;
    }
    if (A == 1) {
      return B == 2 || B == 3;
    }
    return false;
  }

  int edgeTransfer(int /*Src*/, int Dst) const override { return Dst; }

  fact_t applyTransfer(const int &T, const fact_t &In) const override {
    fact_t Out = In;
    Out.insert(T);
    return Out;
  }

  fact_t join(const fact_t &Lhs, const fact_t &Rhs) const override {
    fact_t Out = Lhs;
    Out.insert(Rhs.begin(), Rhs.end());
    return Out;
  }

  bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }

  fact_t bottom() const override { return {}; }

  fact_t initialFact() const override { return {}; }

  std::size_t maxStarIterations() const override { return 1000; }
};

} // namespace elimination_test

namespace elimination_test {

struct NonConvergentDomain {
  using n_t = int;
  using fact_t = int;
  using transfer_t = int;
  using abstract_domain_t = elimination::LegacyProblemDomain<fact_t>;
};

class NonConvergentProblem final
    : public elimination::IntraEliminationProblem<NonConvergentDomain> {
public:
  std::vector<int> nodes() const override { return {0}; }
  int entry() const override { return 0; }
  std::vector<int> succs(int) const override { return {0}; }
  transfer_t edgeTransfer(int, int) const override { return 0; }
  fact_t applyTransfer(const transfer_t &, const fact_t &In) const override {
    return 1 - In;
  }
  fact_t join(const fact_t &, const fact_t &Rhs) const override { return Rhs; }
  bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
    return Lhs == Rhs;
  }
  fact_t bottom() const override { return -1; }
  fact_t initialFact() const override { return 0; }
  std::size_t maxStarIterations() const override { return 100; }
};

} // namespace elimination_test

using elimination_test::APATest;
using elimination_test::factAt;
using elimination_test::NonConvergentDomain;
using elimination_test::NonConvergentProblem;
using elimination_test::ReachabilityProblem;
using elimination_test::ReducibleReachabilityProblem;
using elimination_test::TestDomain;
