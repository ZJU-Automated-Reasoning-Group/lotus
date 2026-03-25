/**
 * @file Andersen.cpp
 * @brief Main implementation of Andersen's flow-insensitive pointer analysis.
 *
 * This file implements the core Andersen pointer analysis algorithm with
 * support for context sensitivity (k-callsite). It orchestrates the three
 * main phases: constraint collection, constraint optimization (HVN/HU), and
 * constraint solving. Also provides command-line options for configuring
 * the analysis behavior.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/Andersen.h"

#include "Alias/AserPTA/PointerAnalysis/Context/CtxTrait.h"
#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/SparrowAA/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "andersen"

using namespace llvm;

STATISTIC(NumValueNodes, "Number of value nodes created");
STATISTIC(NumConstraints, "Number of constraints collected");
STATISTIC(NumAddrOfConstraints, "Number of addr-of constraints");
STATISTIC(NumCopyConstraints, "Number of copy constraints");
STATISTIC(NumLoadConstraints, "Number of load constraints");
STATISTIC(NumStoreConstraints, "Number of store constraints");

// Define option category for Andersen analysis options (non-static so it can be
// used across files)
cl::OptionCategory
    AndersenCategory("Andersen Analysis Options",
                     "Options for configuring Andersen pointer analysis");

cl::opt<bool> DumpDebugInfo("dump-debug",
                            cl::desc("Dump debug info into stderr"),
                            cl::init(false), cl::Hidden,
                            cl::cat(AndersenCategory));
cl::opt<bool> DumpResultInfo("dump-result",
                             cl::desc("Dump result info into stderr"),
                             cl::init(false), cl::Hidden,
                             cl::cat(AndersenCategory));
cl::opt<bool> DumpConstraintInfo("dump-cons",
                                 cl::desc("Dump constraint info into stderr"),
                                 cl::init(false), cl::Hidden,
                                 cl::cat(AndersenCategory));
cl::opt<unsigned>
    AndersenKContext("andersen-k-cs",
                     cl::desc("Context-sensitive Andersen k-callsite (0/1/2)"),
                     cl::init(0), cl::cat(AndersenCategory));
cl::opt<bool> AndersenUseBDDPointsTo(
    "andersen-use-bdd-pts",
    cl::desc("Use BDD-backed points-to sets instead of SparseBitVector"),
    cl::init(false), cl::cat(AndersenCategory));

namespace {

// A lightweight, self-contained K-call-site context to avoid depending on
// the buggy KCallSite equality from AserPTA while still honoring the
// requested level of context sensitivity. Contexts are interned so that
// pointer identity is stable and can be used directly as a map key.
template <unsigned K> struct CallStringContext {
  std::array<const llvm::Instruction *, K> sites{};
  uint8_t size = 0;
  bool isGlobal = false;

  static CallStringContext makeInitial(bool globalFlag) {
    CallStringContext ctx;
    ctx.size = 0;
    ctx.isGlobal = globalFlag;
    return ctx;
  }
};

template <unsigned K> struct CallStringContextHash {
  size_t operator()(const CallStringContext<K> &ctx) const {
    auto begin = ctx.sites.begin();
    return llvm::hash_combine(
        ctx.isGlobal, ctx.size,
        llvm::hash_combine_range(begin, begin + ctx.size));
  }
};

template <unsigned K> struct CallStringContextEq {
  bool operator()(const CallStringContext<K> &lhs,
                  const CallStringContext<K> &rhs) const {
    return lhs.isGlobal == rhs.isGlobal && lhs.size == rhs.size &&
           std::equal(lhs.sites.begin(), lhs.sites.begin() + lhs.size,
                      rhs.sites.begin());
  }
};

template <unsigned K> class CallStringCtxManager {
public:
  using Context = CallStringContext<K>;

  CallStringCtxManager() { reset(); }

  const Context *getInitialCtx() const { return initialCtx; }
  const Context *getGlobalCtx() const { return globalCtx; }

  const Context *evolve(const Context *prev, const llvm::Instruction *I) {
    Context next = *prev;
    next.isGlobal = false;
    if (I != nullptr) {
      if (next.size < K) {
        next.sites[next.size++] = I;
      } else {
        std::move(next.sites.begin() + 1, next.sites.end(), next.sites.begin());
        next.sites[K - 1] = I;
      }
    }
    return intern(std::move(next));
  }

  std::string toString(const Context *ctx, bool detailed) const {
    if (ctx == globalCtx)
      return "<global>";
    if (ctx == initialCtx)
      return "<empty>";

    std::string str;
    llvm::raw_string_ostream os(str);
    os << '<';
    for (unsigned i = 0; i < ctx->size; ++i) {
      const llvm::Instruction *I = ctx->sites[i];
      if (I == nullptr)
        continue;
      if (detailed)
        os << *I;
      else
        os << I;
      if (i + 1 < ctx->size)
        os << "->";
    }
    os << '>';
    return os.str();
  }

  void reset() {
    pool.clear();
    initialCtx = intern(Context::makeInitial(false));
    globalCtx = intern(Context::makeInitial(true));
  }

private:
  using PoolTy = std::unordered_set<Context, CallStringContextHash<K>,
                                    CallStringContextEq<K>>;
  PoolTy pool;
  const Context *initialCtx = nullptr;
  const Context *globalCtx = nullptr;

  const Context *intern(Context ctx) {
    auto inserted = pool.insert(std::move(ctx));
    return &*inserted.first;
  }
};

template <unsigned K> CallStringCtxManager<K> &getCallStringManager() {
  static CallStringCtxManager<K> manager;
  return manager;
}

template <unsigned K> ContextPolicy buildKCallStringPolicy(const char *name) {
  ContextPolicy policy{};
  policy.initialCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().getInitialCtx());
  };
  policy.globalCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().getGlobalCtx());
  };
  policy.evolve = +[](ContextPolicy::Context prev,
                      const llvm::Instruction *I) -> ContextPolicy::Context {
    return static_cast<const void *>(getCallStringManager<K>().evolve(
        static_cast<const typename CallStringCtxManager<K>::Context *>(prev),
        I));
  };
  policy.toString = +[](ContextPolicy::Context ctx, bool detailed) {
    return getCallStringManager<K>().toString(
        static_cast<const typename CallStringCtxManager<K>::Context *>(ctx),
        detailed);
  };
  policy.release = +[]() {
    // Don't reset the manager to avoid invalidating context pointers
    // that may have been captured by existing Andersen objects.
    // The pool will be cleaned up when the program exits.
  };
  policy.k = K;
  policy.name = name;
  return policy;
}

template <typename Ctx>
ContextPolicy buildCtxPolicy(unsigned k, const char *name) {
  (void)k;
  ContextPolicy policy{};
  policy.initialCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(aser::CtxTrait<Ctx>::getInitialCtx());
  };
  policy.globalCtx = +[]() -> ContextPolicy::Context {
    return static_cast<const void *>(aser::CtxTrait<Ctx>::getGlobalCtx());
  };
  policy.evolve = +[](ContextPolicy::Context prev,
                      const llvm::Instruction *I) -> ContextPolicy::Context {
    return static_cast<const void *>(
        aser::CtxTrait<Ctx>::contextEvolve(static_cast<const Ctx *>(prev), I));
  };
  policy.toString = +[](ContextPolicy::Context ctx, bool detailed) {
    return aser::CtxTrait<Ctx>::toString(static_cast<const Ctx *>(ctx),
                                         detailed);
  };
  policy.release = +[]() { aser::CtxTrait<Ctx>::release(); };
  policy.k = k;
  policy.name = name;
  return policy;
}

} // namespace

ContextPolicy makeContextPolicy(unsigned kCallSite) {
  switch (kCallSite) {
  case 1:
    return buildKCallStringPolicy<1>("1-CFA");
  case 2:
    return buildKCallStringPolicy<2>("2-CFA");
  case 0:
    return buildCtxPolicy<aser::NoCtx>(0, "NoCtx");
  default:
    // k > 2 is not supported; warn and fall back to context-insensitive.
    llvm::errs() << "WARNING: Andersen: k-callsite depth " << kCallSite
                 << " is not supported (max is 2); falling back to "
                    "context-insensitive analysis (k=0).\n";
    return buildCtxPolicy<aser::NoCtx>(0, "NoCtx");
  }
}

// Definition of the static ID member declared in Andersen.h.
// Without this definition any ODR-use of Andersen::ID (e.g. passing it to
// getAnalysis<>()) would cause a linker error.
char Andersen::ID = 0;

ContextPolicy getSelectedAndersenContextPolicy() {
  return makeContextPolicy(AndersenKContext);
}

Andersen::Andersen(const Module &module, ContextPolicy policy)
    : ctxPolicy(policy), initialCtx(ctxPolicy.initialCtx()),
      globalCtx(ctxPolicy.globalCtx()) {
  // B3 Fix: use std::atomic to avoid a data race when multiple Andersen
  // objects are constructed concurrently (e.g., in a parallel pass pipeline).
  static std::atomic<int> objectCounter{0};
  int myId = ++objectCounter;
  LOG_INFO("=== Andersen object #{} created ===", myId);
  runOnModule(module);
  LOG_INFO("=== Andersen object #{} analysis completed ===", myId);
}

Andersen::~Andersen() { ctxPolicy.release(); }

void Andersen::getAllAllocationSites(
    std::vector<const llvm::Value *> &allocSites) const {
  nodeFactory.getAllocSites(allocSites);
}

bool Andersen::getPointsToSet(const llvm::Value *v,
                              std::vector<const llvm::Value *> &ptsSet) const {
  AndersPtsSet aggregated;
  if (!getPointsToSet(v, aggregated))
    return false;

  ptsSet.clear();
  DenseSet<const llvm::Value *> uniq;
  for (auto idx : aggregated) {
    if (idx == nodeFactory.getNullObjectNode())
      continue;
    const llvm::Value *val = nodeFactory.getValueForNode(idx);
    if (val && uniq.insert(val).second) {
      ptsSet.push_back(val);
    }
  }
  return true;
}

bool Andersen::getPointsToSet(const llvm::Value *v,
                              AndersPtsSet &ptsSet) const {
  std::vector<NodeIndex> nodes;
  nodeFactory.getValueNodesFor(v, nodes);
  if (nodes.empty())
    return false;

  ptsSet.clear();
  bool sawUnknown = false;
  bool sawKnown = false;
  for (auto n : nodes) {
    if (n == AndersNodeFactory::InvalidIndex ||
        n == nodeFactory.getUniversalPtrNode()) {
      sawUnknown = true;
      continue;
    }
    NodeIndex rep = nodeFactory.getMergeTarget(n);
    auto ptsItr = ptsGraph.find(rep);
    if (ptsItr == ptsGraph.end())
      continue;
    sawKnown = true;
    for (auto idx : ptsItr->second)
      ptsSet.insert(static_cast<unsigned>(idx));
  }

  // B5 Fix: if any node resolved to the universal pointer, the returned set
  // is incomplete (the pointer may point to anything).  Insert the universal
  // object node as a sentinel so callers can detect this condition.
  // Previously the function returned false when sawUnknown && !sawKnown,
  // silently dropping the universal-pointer information.  Now we always
  // return true (with the sentinel) when at least one node was found,
  // whether it was a known node or the universal pointer.
  if (sawUnknown)
    ptsSet.insert(static_cast<unsigned>(nodeFactory.getUniversalObjNode()));

  // Return true if we found any information (known nodes or universal ptr).
  return sawKnown || sawUnknown;
}

bool Andersen::getPointsToSetInContext(const llvm::Value *v,
                                       AndersNodeFactory::CtxKey ctx,
                                       AndersPtsSet &ptsSet) const {
  NodeIndex n = nodeFactory.getValueNodeFor(v, ctx);
  if (n == AndersNodeFactory::InvalidIndex)
    return false;

  // B1 Fix: mirror the universal-pointer sentinel logic from getPointsToSet.
  // If the node resolves to the universal pointer, the pointer may point to
  // anything — insert the universal object sentinel and return true so that
  // callers do not incorrectly conclude "no alias".
  if (n == nodeFactory.getUniversalPtrNode()) {
    ptsSet.clear();
    ptsSet.insert(static_cast<unsigned>(nodeFactory.getUniversalObjNode()));
    return true;
  }

  NodeIndex rep = nodeFactory.getMergeTarget(n);
  auto ptsItr = ptsGraph.find(rep);
  if (ptsItr == ptsGraph.end())
    return false;

  ptsSet.clear();
  for (auto idx : ptsItr->second)
    ptsSet.insert(static_cast<unsigned>(idx));
  return true;
}

bool Andersen::getPointsToSetInContext(
    const llvm::Value *v, AndersNodeFactory::CtxKey ctx,
    std::vector<const llvm::Value *> &ptsSet) const {
  AndersPtsSet aggregated;
  if (!getPointsToSetInContext(v, ctx, aggregated))
    return false;

  ptsSet.clear();
  llvm::DenseSet<const llvm::Value *> uniq;
  for (auto idx : aggregated) {
    if (idx == nodeFactory.getNullObjectNode())
      continue;
    const llvm::Value *val = nodeFactory.getValueForNode(idx);
    if (val && uniq.insert(val).second) {
      ptsSet.push_back(val);
    }
  }
  return true;
}

bool Andersen::runOnModule(const Module &M) {
  LOG_INFO("Starting Andersen analysis on module: {}", M.getName().str());
  visitedFunctions.clear();
  collectConstraints(M);

  // Update statistics after constraint collection
  size_t numConstraints = constraints.size();
  size_t numValueNodes = nodeFactory.getNumNodes();
  NumConstraints = numConstraints;
  NumValueNodes = numValueNodes;
  LOG_INFO("Collected {} constraints and created {} value nodes",
           numConstraints, numValueNodes);
  for (const auto &c : constraints) {
    switch (c.getType()) {
    case AndersConstraint::ADDR_OF:
      ++NumAddrOfConstraints;
      break;
    case AndersConstraint::COPY:
      ++NumCopyConstraints;
      break;
    case AndersConstraint::LOAD:
      ++NumLoadConstraints;
      break;
    case AndersConstraint::STORE:
      ++NumStoreConstraints;
      break;
    }
  }

  if (DumpDebugInfo)
    dumpConstraintsPlainVanilla();

  LOG_INFO("Starting constraint optimization...");
  optimizeConstraints();
  LOG_INFO("Constraint optimization completed");

  if (DumpConstraintInfo)
    dumpConstraints();

  LOG_INFO("Starting constraint solving...");
  solveConstraints();
  LOG_INFO("Andersen analysis completed successfully");

  if (DumpDebugInfo) {
    LOG_DEBUG("");
    dumpPtsGraphPlainVanilla();
  }

  if (DumpResultInfo) {
    nodeFactory.dumpNodeInfo();
    LOG_DEBUG("");
    dumpPtsGraphPlainVanilla();
  }

  return false;
}

void Andersen::dumpConstraint(const AndersConstraint &item) const {
  NodeIndex dest = item.getDest();
  NodeIndex src = item.getSrc();

  switch (item.getType()) {
  case AndersConstraint::COPY: {
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::LOAD: {
    nodeFactory.dumpNode(dest);
    errs() << " = *";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::STORE: {
    errs() << "*";
    nodeFactory.dumpNode(dest);
    errs() << " = ";
    nodeFactory.dumpNode(src);
    break;
  }
  case AndersConstraint::ADDR_OF: {
    nodeFactory.dumpNode(dest);
    errs() << " = &";
    nodeFactory.dumpNode(src);
  }
  }

  errs() << "\n";
}

void Andersen::dumpConstraints() const {
  LOG_DEBUG("\n----- Constraints -----");
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  for (auto const &item : constraints)
    dumpConstraint(item);
#endif
  LOG_DEBUG("----- End of Print -----");
}

void Andersen::dumpConstraintsPlainVanilla() const {
#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
  for (auto const &item : constraints) {
    LOG_DEBUG("{} {} {} 0", item.getType(), item.getDest(), item.getSrc());
  }
#endif
}

void Andersen::dumpPtsGraphPlainVanilla() const {
  for (unsigned i = 0, e = nodeFactory.getNumNodes(); i < e; ++i) {
    NodeIndex rep = nodeFactory.getMergeTarget(i);
    auto ptsItr = ptsGraph.find(rep);
    if (ptsItr != ptsGraph.end()) {
      std::stringstream ss;
      ss << i;
      for (auto v : ptsItr->second)
        ss << " " << v;
      LOG_DEBUG("{}", ss.str());
    }
  }
}
