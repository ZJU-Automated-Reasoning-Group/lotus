// This header is a compatibility shim.
//
// The original Core/IntraMonoSolver.h defined a second, simpler
// mono::IntraMonoSolver class template that conflicted with the richer
// implementation in Solver/IntraSolver.h (B1 fix).  Having two definitions
// of the same class template in the same namespace is an ODR violation and
// caused ambiguity for clients that included both headers.
//
// The authoritative implementation is now exclusively in:
//   include/Dataflow/Mono/Solver/IntraSolver.h
//
// This file simply re-exports that header so that any existing #include of
// "Dataflow/Mono/Core/IntraMonoSolver.h" continues to compile without change.

#pragma once
#include "Dataflow/Mono/Solver/IntraSolver.h"
