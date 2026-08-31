#pragma once

/**
 * @file MHPAnalysisTest.cpp
 * @brief Simplified unit tests for MHP Analysis
 */

#include "Concurrency/MHP/MHPAnalysis.h"

#include "Concurrency/MHP/HappensBeforeAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class MHPAnalysisTest : public lotus::unittest::LlvmModuleTest {};
