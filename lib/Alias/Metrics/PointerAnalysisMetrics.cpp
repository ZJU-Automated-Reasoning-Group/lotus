/**
 * @file PointerAnalysisMetrics.cpp
 * @brief Collect pointer analysis metrics from AliasAnalysisWrapper for
 * comparison
 */

#include "Alias/Metrics/PointerAnalysisMetrics.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <algorithm>
#include <set>
#include <vector>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

using namespace llvm;
using namespace lotus;

namespace {

/// Collect all pointer-type values that are meaningful to track (e.g. in F).
void collectPointerValues(Function &F, std::vector<const Value *> &out) {
  for (Argument &A : F.args())
    if (A.getType()->isPointerTy())
      out.push_back(&A);
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (I.getType()->isPointerTy())
        out.push_back(&I);
}

/// Count direct call edges in M (each direct call = one edge).
uint64_t countDirectCallEdges(Module &M) {
  uint64_t n = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          if (CB->isIndirectCall())
            continue;
          if (Function *Callee = CB->getCalledFunction())
            n++;
        }
      }
  }
  return n;
}

/// Canonical pair for deduplication: (min by address, max by address).
using PtrPair = std::pair<const Value *, const Value *>;
PtrPair canonicalPair(const Value *a, const Value *b) {
  return a < b ? PtrPair(a, b) : PtrPair(b, a);
}

/// Collect pointer operands at an instruction (including result if pointer).
void getPointerOperands(const Instruction &I, std::vector<const Value *> &out) {
  out.clear();
  if (I.getType()->isPointerTy())
    out.push_back(&I);
  for (const Use &U : I.operands()) {
    const Value *V = U.get();
    if (V && V->getType()->isPointerTy())
      out.push_back(V);
  }
}

} // namespace

void lotus::collectMetricsFromWrapper(AliasAnalysisWrapper &aa, Module &M,
                                      PointerAnalysisMetrics &out,
                                      CollectMetricsOptions options) {
  out = PointerAnalysisMetrics{};

  if (!aa.isInitialized())
    return;

  std::vector<const Value *> pointers;
  std::vector<const llvm::Function *> callees;
  uint64_t totalPtsSize = 0;
  uint64_t numWithPts = 0;
  uint64_t maxPts = 0;
  std::vector<uint64_t> ptsSizes; // for median

  // --- Points-to set size statistics ---
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    pointers.clear();
    collectPointerValues(F, pointers);
    for (const Value *ptr : pointers) {
      std::vector<const Value *> ptsSet;
      uint64_t sz = 0;
      if (aa.getPointsToSet(ptr, ptsSet)) {
        sz = static_cast<uint64_t>(ptsSet.size());
      } else {
        size_t ptsz = 0;
        if (!aa.getPointsToSetSize(ptr, ptsz))
          continue;
        sz = static_cast<uint64_t>(ptsz);
      }
      totalPtsSize += sz;
      numWithPts++;
      if (sz > maxPts)
        maxPts = sz;
      ptsSizes.push_back(sz);
    }
  }

  out.num_pointers_tracked = numWithPts;
  out.total_points_to_size = totalPtsSize;
  out.max_points_to_size = maxPts;
  if (numWithPts > 0) {
    out.avg_points_to_size =
        static_cast<double>(totalPtsSize) / static_cast<double>(numWithPts);
    if (!ptsSizes.empty()) {
      std::sort(ptsSizes.begin(), ptsSizes.end());
      if (ptsSizes.size() % 2 == 0) {
        out.median_points_to_size =
            0.5 * (static_cast<double>(ptsSizes[ptsSizes.size() / 2 - 1]) +
                   static_cast<double>(ptsSizes[ptsSizes.size() / 2]));
      } else {
        out.median_points_to_size =
            static_cast<double>(ptsSizes[ptsSizes.size() / 2]);
      }
    }
  }

  // --- Direct call edges ---
  out.num_direct_call_edges = countDirectCallEdges(M);

  // --- Indirect call sites and resolved targets ---
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        CallBase *CB = dyn_cast<CallBase>(&I);
        if (!CB || !CB->isIndirectCall())
          continue;
        out.num_indirect_call_sites++;
        aa.getIndirectCallTargets(CB, callees);
        uint64_t n = callees.size();
        out.num_indirect_call_edges += n;
        if (n > 1)
          out.num_poly_indirect_calls++;
      }
    }
  }
  if (out.num_indirect_call_sites > 0) {
    out.avg_targets_per_indirect =
        static_cast<double>(out.num_indirect_call_edges) /
        static_cast<double>(out.num_indirect_call_sites);
  }

  // --- Alias-pair metrics (use-site pairs, capped) ---
  if (options.max_alias_pairs > 0) {
    std::vector<const Value *> ptrs;
    std::set<PtrPair> seen;
    uint64_t noAlias = 0, mustAlias = 0, mayAlias = 0, partialAlias = 0;
    uint64_t queried = 0;
    const uint64_t cap = options.max_alias_pairs;

    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
          getPointerOperands(I, ptrs);
          for (size_t i = 0; i < ptrs.size(); ++i) {
            for (size_t j = i + 1; j < ptrs.size(); ++j) {
              const Value *v1 = ptrs[i];
              const Value *v2 = ptrs[j];
              if (v1 == v2)
                continue;
              PtrPair p = canonicalPair(v1, v2);
              if (seen.count(p))
                continue;
              if (queried >= cap)
                goto done_alias;
              seen.insert(p);
              queried++;
              switch (aa.query(v1, v2)) {
              case AliasResult::NoAlias:
                noAlias++;
                break;
              case AliasResult::MustAlias:
                mustAlias++;
                break;
              case AliasResult::MayAlias:
                mayAlias++;
                break;
              case AliasResult::PartialAlias:
                partialAlias++;
                break;
              default:
                mayAlias++;
                break;
              }
            }
          }
        }
      }
    }
  done_alias:
    out.num_alias_pairs_queried = queried;
    out.num_no_alias = noAlias;
    out.num_must_alias = mustAlias;
    out.num_may_alias = mayAlias;
    out.num_partial_alias = partialAlias;
  }
}
