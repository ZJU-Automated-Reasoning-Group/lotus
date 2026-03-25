// Explicit instantiations for RegexDag types used by ProcedureResources and
// DagInterpreter.
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/RegexDag/BackwardClosedOverlay.h"
#include "Verification/Sifa/RegexDag/RegexDag.h"
#include "Verification/Sifa/RegexDag/RegexDagNode.h"

template class lotus::sifa::RegexDag<lotus::sifa::Transition>;
template class lotus::sifa::RegexDagNode<lotus::sifa::Transition>;
template class lotus::sifa::BackwardClosedOverlay<lotus::sifa::Transition>;
