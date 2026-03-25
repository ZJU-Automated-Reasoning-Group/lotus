// Explicit instantiations for Summarizer types used by Sifa and SifaSymAbs.
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/SymAbs/SifaSymAbsDomain.h"

template class lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition,
                                                   bool>;
template class lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition,
                                                   lotus::sifa::SymAbsState>;
