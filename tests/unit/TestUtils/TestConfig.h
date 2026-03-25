#ifndef LOTUS_UNITTEST_TESTUTILS_TESTCONFIG_H_
#define LOTUS_UNITTEST_TESTUTILS_TESTCONFIG_H_

#include "llvm/ADT/StringRef.h"
#include "gtest/gtest.h"
#include <string>

namespace lotus { namespace unittest {

// Path to LLVM test files (used for regression tests with .ll files)
static const std::string PathToLLTestFiles = 
    std::string(LOTUS_BUILD_DIR) + "/tests/regress/llvm_test_code/";

// Path to text test files
static const std::string PathToTxtTestFiles = 
    std::string(LOTUS_BUILD_DIR) + "/tests/regress/text_test_code/";

// Path to JSON test files (for configuration tests)
static const std::string PathToJSONTestFiles = 
    std::string(LOTUS_SRC_DIR) + "/tests/regress/json_test_code/";

#define LOTUS_BUILD_SUBFOLDER(SUB) \
  (std::string(LOTUS_BUILD_DIR) + "/tests/regress/llvm_test_code/" SUB)

// Utility macro for skipping tests conditionally
#ifdef GTEST_SKIP
#define LOTUS_SKIP_TEST(...) __VA_ARGS__
#else
#define LOTUS_SKIP_TEST(...)
#endif

} // namespace unittest
 } // namespace lotus

#endif // LOTUS_UNITTEST_TESTUTILS_TESTCONFIG_H_
