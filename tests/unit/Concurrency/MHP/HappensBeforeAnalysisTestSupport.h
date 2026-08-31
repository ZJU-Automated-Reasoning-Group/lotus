#pragma once

#include "Concurrency/MHP/HappensBeforeAnalysis.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;
using namespace lotus::unittest;

class HappensBeforeAnalysisTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};

