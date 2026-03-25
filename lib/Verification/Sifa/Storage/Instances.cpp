// Explicit instantiations for Storage types used by Sifa and SifaSymAbs.
#include "llvm/IR/BasicBlock.h"

#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"

template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *, bool>;
template class lotus::sifa::MapBasedStorage<const llvm::BasicBlock *,
                                            lotus::sifa::SymAbsState>;
