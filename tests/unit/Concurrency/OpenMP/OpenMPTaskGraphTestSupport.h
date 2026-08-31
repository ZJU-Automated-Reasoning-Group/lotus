#pragma once

#include "Concurrency/OpenMP/OpenMPTaskGraph.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace OpenMP;

class OpenMPTaskGraphTest : public lotus::unittest::LlvmModuleTest {};
