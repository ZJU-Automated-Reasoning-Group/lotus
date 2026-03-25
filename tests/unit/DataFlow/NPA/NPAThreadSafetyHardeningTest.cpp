#include "Dataflow/NPA/Domains/BitVectorDomain.h"
#include "Dataflow/NPA/Domains/GenKillDomain.h"
#include "Dataflow/NPA/Domains/TaintTransferDomain.h"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <llvm/ADT/APInt.h>
#include <gtest/gtest.h>

namespace {

template <typename Fn> std::vector<int> runOnThreads(unsigned count, Fn fn) {
  std::vector<int> results(count, 0);
  std::vector<std::thread> threads;
  threads.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    threads.emplace_back([&, i] { results[i] = fn() ? 1 : 0; });
  }
  for (auto &thread : threads)
    thread.join();
  return results;
}

template <typename LhsFn, typename RhsFn>
bool runConcurrentPair(LhsFn lhs_fn, RhsFn rhs_fn) {
  std::mutex mutex;
  std::condition_variable cv;
  unsigned ready = 0;
  bool lhs_ok = false;
  bool rhs_ok = false;

  auto worker = [&](const std::function<bool()> &fn, bool &ok) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ++ready;
      cv.wait(lock, [&] { return ready == 2; });
      cv.notify_all();
    }
    ok = fn();
  };

  std::thread lhs([&] { worker(lhs_fn, lhs_ok); });
  std::thread rhs([&] { worker(rhs_fn, rhs_ok); });
  lhs.join();
  rhs.join();
  return lhs_ok && rhs_ok;
}

} // namespace

TEST(NPAThreadSafetyHardening, BitVectorWidthScopesRemainIsolatedAcrossThreads) {
  auto run_case = [](unsigned width, unsigned expected_population) {
    npa::BitVectorDomain::WidthScope scope(width);
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
      auto zero = npa::BitVectorDomain::zero();
      auto one = npa::BitVectorDomain::one();
      if (zero.getBitWidth() != width || one.getBitWidth() != width)
        return false;
      if (one.countPopulation() != expected_population)
        return false;
      if (npa::BitVectorDomain::combine(zero, one) != one)
        return false;
    }
    return true;
  };

  EXPECT_TRUE(runConcurrentPair(
      [&] { return run_case(7, 7); }, [&] { return run_case(13, 13); }));
}

TEST(NPAThreadSafetyHardening, GenKillWidthScopesRemainIsolatedAcrossThreads) {
  auto run_case = [](unsigned width) {
    npa::GenKillTransferDomain::WidthScope scope(width);
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
      auto zero = npa::GenKillTransferDomain::zero();
      auto one = npa::GenKillTransferDomain::one();
      if (zero.first.getBitWidth() != width || zero.second.getBitWidth() != width)
        return false;
      if (one.first.getBitWidth() != width || one.second.getBitWidth() != width)
        return false;
      if (zero.first.countPopulation() != width || zero.second.countPopulation() != 0)
        return false;
      if (!npa::GenKillTransferDomain::equal(
              npa::GenKillTransferDomain::combine(one, zero), one)) {
        return false;
      }
    }
    return true;
  };

  EXPECT_TRUE(
      runConcurrentPair([&] { return run_case(11); }, [&] { return run_case(17); }));
}

TEST(NPAThreadSafetyHardening, TaintWidthScopesRemainIsolatedAcrossThreads) {
  auto run_case = [](unsigned width) {
    npa::TaintTransferDomain::WidthScope scope(width);
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
      auto zero = npa::TaintTransferDomain::zero();
      auto one = npa::TaintTransferDomain::one();
      if (zero.gen.getBitWidth() != width || one.gen.getBitWidth() != width)
        return false;
      if (one.rel.size() != width || one.rel.front().getBitWidth() != width)
        return false;
      npa::TaintTransferDomain::addEdge(one, 0, width - 1);
      npa::TaintTransferDomain::addGen(one, width - 1);
      llvm::APInt input(width, 0);
      input.setBit(0);
      llvm::APInt output = npa::TaintTransferDomain::apply(one, input);
      if (!output[width - 1])
        return false;
    }
    return true;
  };

  EXPECT_TRUE(
      runConcurrentPair([&] { return run_case(7); }, [&] { return run_case(19); }));
}

TEST(NPAThreadSafetyHardening, SafeCoreDomainsSupportConcurrentReadOnlyOps) {
  npa::TaintTransferDomain::WidthScope taint_scope(4);
  npa::BitVectorDomain::WidthScope bit_scope(4);
  npa::GenKillTransferDomain::WidthScope gen_kill_scope(4);

  auto transfer = npa::TaintTransferDomain::one();
  npa::TaintTransferDomain::addEdge(transfer, 0, 1);
  npa::TaintTransferDomain::addEdge(transfer, 1, 2);
  npa::TaintTransferDomain::addGen(transfer, 3);
  const auto composed = npa::TaintTransferDomain::extend(transfer, transfer);
  llvm::APInt input(4, 0);
  input.setBit(0);
  const llvm::APInt expectedTaint =
      npa::TaintTransferDomain::apply(composed, input);

  llvm::APInt bitsA(4, 0);
  bitsA.setBit(0);
  bitsA.setBit(2);
  llvm::APInt bitsB(4, 0);
  bitsB.setBit(1);
  bitsB.setBit(2);
  const llvm::APInt expectedBitVector =
      npa::BitVectorDomain::combine(bitsA, bitsB);

  npa::GenKillTransferDomain::value_type genKillA{
      llvm::APInt(4, 0b0011), llvm::APInt(4, 0b0100)};
  npa::GenKillTransferDomain::value_type genKillB{
      llvm::APInt(4, 0b1000), llvm::APInt(4, 0b0001)};
  const auto expectedGenKill =
      npa::GenKillTransferDomain::extend(genKillA, genKillB);

  auto results = runOnThreads(4, [&] {
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
      if (npa::TaintTransferDomain::apply(composed, input) != expectedTaint)
        return false;
      if (!npa::TaintTransferDomain::equal(
              npa::TaintTransferDomain::extend(transfer, transfer), composed)) {
        return false;
      }
      if (npa::BitVectorDomain::combine(bitsA, bitsB) != expectedBitVector)
        return false;
      if (npa::GenKillTransferDomain::extend(genKillA, genKillB) !=
          expectedGenKill) {
        return false;
      }
    }
    return true;
  });

  EXPECT_TRUE(
      std::all_of(results.begin(), results.end(), [](int ok) { return ok; }));
}
