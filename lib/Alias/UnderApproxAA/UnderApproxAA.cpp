/**
 * @file UnderApproxAA.cpp
 * @brief Implementation of under-approximation alias analysis
 *
 * This file implements the LLVM AAResult interface for under-approximation
 * alias analysis. The analysis uses union-find with congruence closure to
 * identify must-alias relationships within functions.
 *
 * Key design decisions:
 * - Per-function caching: Each function's EquivDB is built once and reused
 * - Intra-procedural only: Cross-function queries remain MayAlias/unknown
 * - Sound under-approximation: Returns MustAlias only when guaranteed
 * - Optional MemorySSA: Store-load forwarding for more precision (sound)
 * - Optional DominatorTree: Single-store alloca forwarding for more precision
 *   (sound)
 */

#include "Alias/UnderApproxAA/UnderApproxAA.h"

#include "Alias/UnderApproxAA/EquivDB.h"

#include <unordered_map>

#include <llvm/ADT/Hashing.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace UnderApprox;

// ---------------------------------------------------------------------------
// Per-function cache – built lazily, reused by all subsequent queries
// ---------------------------------------------------------------------------

namespace {
/// Cache key: combines function pointer with analysis providers
/// This ensures different provider configurations get different caches
struct CacheKey {
  const Function *F;
  UnderApproxAA::MemorySSAProvider MSSAProv;
  UnderApproxAA::DominatorTreeProvider DTProv;

  bool operator==(const CacheKey &Other) const {
    return F == Other.F && MSSAProv == Other.MSSAProv && DTProv == Other.DTProv;
  }
};

/// Hash function for CacheKey
struct CacheKeyHash {
  size_t operator()(const CacheKey &K) const {
    return hash_combine(K.F, K.MSSAProv, K.DTProv);
  }
};

/// Cache type: maps each cache key to its equivalence database
struct CacheEntry {
  uint64_t Fingerprint = 0;
  std::unique_ptr<EquivDB> DB;
};

using CacheTy = std::unordered_map<CacheKey, CacheEntry, CacheKeyHash>;

/// Global per-function EquivDB cache. Entries are rebuilt lazily if the
/// function fingerprint changes.
static CacheTy EquivCache;

uint64_t computeFunctionFingerprint(const Function &F) {
  hash_code H = hash_value(F.getName());
  H = hash_combine(H, F.arg_size(), F.size());

  for (const Argument &A : F.args())
    H = hash_combine(H, A.getArgNo(), A.getType());

  for (const BasicBlock &BB : F) {
    H = hash_combine(H, BB.size(), BB.hasName() ? BB.getName() : StringRef{});
    for (const Instruction &I : BB) {
      H = hash_combine(H, I.getOpcode(), I.getType(), I.getNumOperands());

      if (const auto *GEP = dyn_cast<GetElementPtrInst>(&I))
        H = hash_combine(H, GEP->getSourceElementType());
      if (const auto *CB = dyn_cast<CallBase>(&I))
        H = hash_combine(H, CB->getCalledOperand(), CB->arg_size());
      for (const Value *Op : I.operands())
        H = hash_combine(H, Op, Op->getType());
    }
  }

  return static_cast<uint64_t>(H);
}

/// Extract the parent function of a value
/// @param V The value (instruction or argument) to query
/// @return The function containing V, or nullptr if V is not function-scoped
///
/// This helper is used to determine which function's EquivDB should be
/// used for a query. Values must be within the same function to be compared.
const Function *getParentFunction(const Value *V) {
  // Instructions are contained in basic blocks, which are in functions
  if (auto *I = dyn_cast<Instruction>(V))
    return I->getParent()->getParent();
  // Arguments directly belong to functions
  if (auto *A = dyn_cast<Argument>(V))
    return A->getParent();
  // Global values, constants, etc. are not function-scoped
  return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor and Destructor
// ---------------------------------------------------------------------------

UnderApproxAA::UnderApproxAA(Module &M) : _module(M) {
  // Note: The EquivDB instances are created lazily on first query
  // to avoid building analysis for functions that are never queried.
}

UnderApproxAA::~UnderApproxAA() {
  // The cache is static, so it persists across analysis instances.
  // Entries rebuild automatically when the function fingerprint changes.
}

void UnderApproxAA::invalidateCache(const llvm::Function *F) {
  // Erase every cache entry whose function pointer matches F.
  // There may be multiple entries (one per provider configuration).
  for (auto It = EquivCache.begin(); It != EquivCache.end();) {
    if (It->first.F == F)
      It = EquivCache.erase(It);
    else
      ++It;
  }
}

void UnderApproxAA::invalidateAllCaches() { EquivCache.clear(); }

// ---------------------------------------------------------------------------
// AAResult interface implementation
// ---------------------------------------------------------------------------

AliasResult UnderApproxAA::alias(const MemoryLocation &L1,
                                 const MemoryLocation &L2) {
  // Extract pointer values from memory locations and delegate to mustAlias.
  if (!mustAlias(L1.Ptr, L2.Ptr))
    // Unknown — we cannot prove they alias, but we also cannot prove they
    // don't. Returning NoAlias here would violate the AAResult contract
    // (NoAlias means *definitely* no alias). Return MayAlias instead.
    return AliasResult::MayAlias;

  // The pointers must alias at the pointer level.  Now check size
  // compatibility: if both sizes are known and non-zero, the locations
  // must overlap (same start address, both cover at least one byte).
  // If either size is unknown (LocationSize::unknown()), we conservatively
  // return MustAlias — the pointer-level guarantee is still valid.
  //
  // We do NOT return PartialAlias here because an under-approximation AA
  // should only assert MustAlias when it is certain; for any uncertainty
  // we fall back to MayAlias.
  const LocationSize &S1 = L1.Size;
  const LocationSize &S2 = L2.Size;

  // If both sizes are precisely known and at least one is zero, the regions
  // are zero-length and technically do not overlap — return MayAlias to be
  // conservative (zero-size accesses are unusual; don't claim MustAlias).
  if (S1.isPrecise() && S2.isPrecise()) {
    if (S1.getValue() == 0 || S2.getValue() == 0)
      return AliasResult::MayAlias;
  }

  return AliasResult::MustAlias;
}

AliasResult UnderApproxAA::query(const Value *V1, const Value *V2) {
  return mustAlias(V1, V2) ? AliasResult::MustAlias : AliasResult::MayAlias;
}

bool UnderApproxAA::mustAlias(const Value *V1, const Value *V2) {
  // Early exit: ensure both values are valid pointers
  if (!isValidPointerQuery(V1, V2))
    return false;

  // Extract parent functions for both values
  const Function *F1 = getParentFunction(V1);
  const Function *F2 = getParentFunction(V2);

  const Function *QueryF = nullptr;
  if (F1 && F2) {
    // True cross-function queries are not handled.
    if (F1 != F2)
      return false;
    QueryF = F1;
  } else if (F1) {
    QueryF = F1;
  } else if (F2) {
    QueryF = F2;
  }

  // Safety check: no function context to build an EquivDB
  if (!QueryF)
    return false;

  // Safety check: ensure function has a parent module before building EquivDB
  if (!QueryF->getParent()) {
    return false;
  }

  // Get optional analyses from providers
  MemorySSA *MSSA = nullptr;
  DominatorTree *DT = nullptr;

  if (MSSAProvider) {
    MSSA = MSSAProvider(*const_cast<Function *>(QueryF));
  }
  if (DTProvider) {
    DT = DTProvider(*const_cast<Function *>(QueryF));
  }

  // Create cache key
  CacheKey Key{QueryF, MSSAProvider, DTProvider};

  // Lazy initialization: build EquivDB for QueryF on first query
  // The cache ensures we only build it once per function+configuration
  const uint64_t Fingerprint = computeFunctionFingerprint(*QueryF);
  auto &Entry = EquivCache[Key];
  if (!Entry.DB || Entry.Fingerprint != Fingerprint) {
    // Note: const_cast is necessary because EquivDB constructor takes
    // a non-const Function&, but we only have const Function* from
    // getParentFunction. This is safe because EquivDB only reads the IR.
    try {
      Entry.DB =
          std::make_unique<EquivDB>(*const_cast<Function *>(QueryF), MSSA, DT);
      Entry.Fingerprint = Fingerprint;
    } catch (...) {
      // If EquivDB construction fails, return false (no must-alias)
      return false;
    }
  }

  // Safety check: ensure EquivDB was successfully created
  if (!Entry.DB)
    return false;

  // Query the equivalence database for this function
  return Entry.DB->mustAlias(V1, V2);
}

bool UnderApproxAA::isValidPointerQuery(const Value *v1,
                                        const Value *v2) const {
  // Validate that both values are non-null and have pointer types
  // This prevents invalid queries and avoids crashes
  return v1 && v2 && v1->getType()->isPointerTy() &&
         v2->getType()->isPointerTy();
}
