#pragma once

/**
 * @file LockSetAnalysisTest.cpp
 * @brief Unit tests for Lock Set Analysis
 */

#include "Concurrency/LockSet/LockSetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/Utils/RAIILockTracker.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>

using namespace llvm;
using namespace mhp;
using namespace lotus::unittest;

class LockSetAnalysisTest : public lotus::unittest::LlvmModuleTest {};

