// Explicit instantiations for Worklist types used by DagInterpreter and
// IcfgInterpreter.
#include "llvm/IR/Function.h"

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/RegexDag/RegexDagNode.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Worklist/PriorityWorklist.h"

template class lotus::sifa::PriorityWorklist<
    lotus::sifa::RegexDagNode<lotus::sifa::Transition> *, bool>;
template class lotus::sifa::PriorityWorklist<
    lotus::sifa::RegexDagNode<lotus::sifa::Transition> *,
    lotus::sifa::SymAbsState>;
template class lotus::sifa::PriorityWorklist<const llvm::Function *, bool>;
