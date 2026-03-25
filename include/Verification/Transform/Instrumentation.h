#pragma once

#include "llvm/Pass.h"

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createInitializeUninitializedPass();
llvm::Pass *createMakeNondetPass();
llvm::Pass *createPrepareOverflowsPass();
llvm::Pass *createBreakCritLoopsPass();
llvm::Pass *createDeleteUndefinedPass();
llvm::Pass *createRemoveConstantExprsPass();
llvm::Pass *createInternalizeGlobalsPass();
llvm::Pass *createRemoveErrorCallsPass();
llvm::Pass *createInstrumentAllocPass();
llvm::Pass *createInstrumentAllocNeverFailsPass();
llvm::Pass *createRemoveInfiniteLoopsPass();
llvm::Pass *createBreakInfiniteLoopsPass();
llvm::Pass *createFlattenLoopsPass();
llvm::Pass *createInstrumentNonterminationPass();
llvm::Pass *createRemoveReadOnlyAttrPass();
llvm::Pass *createRenameVerifierFunsPass();
llvm::Pass *createReplaceLifetimeMarkersPass();
llvm::Pass *createMarkVolatilePass();
llvm::Pass *createFindExitsPass();
llvm::Pass *createDummyMarkerPass();
llvm::Pass *createUnrollingPass();
llvm::Pass *createExplicitConsdesPass();
llvm::Pass *createDeleteCallsPass();
llvm::Pass *createReplaceVerifierAtomicPass();

} // namespace transform
} // namespace verification
} // namespace lotus
