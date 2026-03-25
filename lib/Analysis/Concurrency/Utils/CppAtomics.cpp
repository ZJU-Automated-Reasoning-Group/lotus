/**
 * @file CppAtomics.cpp
 * @brief C++ Atomics Recognition Implementation (C++11/14/17/20)
 *
 * @author rainofetime
 * @date 2025-2026
 */

#include "Analysis/Concurrency/Utils/CppAtomics.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Instructions.h>

namespace CppAtomics {

// Helper to convert LLVM's AtomicOrdering to our enum
static MemoryOrder fromLLVMOrdering(llvm::AtomicOrdering ordering) {
    switch (ordering) {
        case llvm::AtomicOrdering::NotAtomic:
            return MemoryOrder::NotAtomic;
        case llvm::AtomicOrdering::Unordered: // Map Unordered to Relaxed
        case llvm::AtomicOrdering::Monotonic: // Map Monotonic to Relaxed
            return MemoryOrder::Relaxed;
#if LLVM_VERSION_MAJOR >= 15
        case llvm::AtomicOrdering::Consume:
            return MemoryOrder::Acquire;
#endif
        case llvm::AtomicOrdering::Acquire:
            return MemoryOrder::Acquire;
        case llvm::AtomicOrdering::Release:
            return MemoryOrder::Release;
        case llvm::AtomicOrdering::AcquireRelease:
            return MemoryOrder::AcquireRelease;
        case llvm::AtomicOrdering::SequentiallyConsistent:
            return MemoryOrder::SequentiallyConsistent;
        default:
            return MemoryOrder::NotAtomic;
    }
}

bool isAtomic(const llvm::Instruction *inst) {
    if (!inst) return false;
    return inst->isAtomic();
}

MemoryOrder getMemoryOrder(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) {
        return MemoryOrder::NotAtomic;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        return fromLLVMOrdering(load->getOrdering());
    }
    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        return fromLLVMOrdering(store->getOrdering());
    }
    if (const auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(inst)) {
        return fromLLVMOrdering(rmw->getOrdering());
    }
    if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
        return fromLLVMOrdering(cmpxchg->getSuccessOrdering());
    }
    if (const auto *fence = llvm::dyn_cast<llvm::FenceInst>(inst)) {
        return fromLLVMOrdering(fence->getOrdering());
    }

    return MemoryOrder::NotAtomic;
}

const llvm::Value *getAtomicPointer(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) {
        return nullptr;
    }

    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
        return load->getPointerOperand();
    }
    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        return store->getPointerOperand();
    }
    if (const auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(inst)) {
        return rmw->getPointerOperand();
    }
    if (const auto *cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(inst)) {
        return cmpxchg->getPointerOperand();
    }

    // Fence instructions do not operate on a specific pointer
    return nullptr;
}

bool isLockFree(const llvm::Instruction *inst) {
    // Lock-freedom is target- and type-dependent. The previous implementation
    // incorrectly used volatility as a proxy, which is unrelated to
    // lock-freedom and could silently mislead callers. Keep this helper
    // conservative until a target-aware query is implemented.
    (void)inst;
    return false;
}

bool isStore(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::StoreInst>(inst) || llvm::isa<llvm::AtomicRMWInst>(inst) || llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

bool isLoad(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::AtomicRMWInst>(inst) || llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

bool isReadModifyWrite(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::AtomicRMWInst>(inst) ||
           llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

bool isCompareExchange(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;
    return llvm::isa<llvm::AtomicCmpXchgInst>(inst);
}

bool isFence(const llvm::Instruction *inst) {
    return llvm::isa<llvm::FenceInst>(inst);
}

// Memory ordering property checks for synchronization analysis
bool hasAcquireSemantics(const llvm::Instruction *inst) {
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::Acquire ||
           order == MemoryOrder::AcquireRelease ||
           order == MemoryOrder::SequentiallyConsistent;
}

bool hasReleaseSemantics(const llvm::Instruction *inst) {
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::Release ||
           order == MemoryOrder::AcquireRelease ||
           order == MemoryOrder::SequentiallyConsistent;
}

bool hasSequentialConsistency(const llvm::Instruction *inst) {
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::SequentiallyConsistent;
}

bool isRelaxed(const llvm::Instruction *inst) {
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::Relaxed;
}

// Synchronizes-with relationship helpers
bool canSynchronizeWith(const llvm::Instruction *release, const llvm::Instruction *acquire) {
    if (!release || !acquire) return false;
    if (isFence(release) || isFence(acquire)) return false;

    // Release operation must have release semantics
    if (!hasReleaseSemantics(release)) return false;

    // Acquire operation must have acquire semantics
    if (!hasAcquireSemantics(acquire)) return false;

    // Both must operate on the same memory location
    const llvm::Value *relPtr = getAtomicPointer(release);
    const llvm::Value *acqPtr = getAtomicPointer(acquire);

    if (!relPtr || !acqPtr) return false;

    // Direct atomic synchronization is exact-location only here. Callers that
    // want alias-aware matching should add that policy at a higher layer.
    return relPtr->stripPointerCasts() == acqPtr->stripPointerCasts();
}

bool participatesInReleaseSequence(const llvm::Instruction *inst) {
    if (!isAtomic(inst)) return false;

    // An operation participates in a release sequence if it's:
    // 1. A release operation, or
    // 2. An atomic RMW operation on the same location
    return hasReleaseSemantics(inst) || isReadModifyWrite(inst);
}

// Fence analysis
bool isFenceAcquire(const llvm::Instruction *inst) {
    if (!isFence(inst)) return false;
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::Acquire;
}

bool isFenceRelease(const llvm::Instruction *inst) {
    if (!isFence(inst)) return false;
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::Release;
}

bool isFenceAcqRel(const llvm::Instruction *inst) {
    if (!isFence(inst)) return false;
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::AcquireRelease;
}

bool isFenceSeqCst(const llvm::Instruction *inst) {
    if (!isFence(inst)) return false;
    MemoryOrder order = getMemoryOrder(inst);
    return order == MemoryOrder::SequentiallyConsistent;
}

// Helper to get human-readable memory order string
const char* memoryOrderToString(MemoryOrder order) {
    switch (order) {
        case MemoryOrder::NotAtomic: return "not_atomic";
        case MemoryOrder::Relaxed: return "relaxed";
        case MemoryOrder::Consume: return "consume";
        case MemoryOrder::Acquire: return "acquire";
        case MemoryOrder::Release: return "release";
        case MemoryOrder::AcquireRelease: return "acq_rel";
        case MemoryOrder::SequentiallyConsistent: return "seq_cst";
        default: return "unknown";
    }
}

} // namespace CppAtomics
