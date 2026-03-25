//===- SaberOptions.cpp -- Saber checker options -------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// Command-line option definitions.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SaberOptions.h"

#include <llvm/Support/CommandLine.h>

using namespace llvm;

namespace lotus {
namespace analysis {

// Define option category
static cl::OptionCategory
    SaberCategory("Saber Options", "Options for Saber source-sink checker");

// Define options
cl::opt<bool> SaberFullSVFG("saber-full-svfg",
                            cl::desc("Use the full, unoptimized SABER SVFG "
                                     "(default uses the optimized "
                                     "compatibility graph)"),
                            cl::init(false), cl::cat(SaberCategory));

cl::opt<unsigned>
    SaberCxtLimit("saber-cxt-limit",
                  cl::desc("Max call-string context length (k-limit; beyond "
                           "this contexts merge to avoid explosion)"),
                  cl::init(3u), cl::cat(SaberCategory));

cl::opt<unsigned>
    SaberMaxStepInWrapper("saber-max-step-wrapper",
                          cl::desc("Max steps in wrapper detection"),
                          cl::init(10u), cl::cat(SaberCategory));

cl::opt<unsigned> SaberMaxForwardItems(
    "saber-max-forward-items",
    cl::desc("Max (node,context) items per source in forward traversal (0 = no "
             "limit); safety cap only"),
    cl::init(0u), cl::cat(SaberCategory));

cl::opt<unsigned> SaberZ3Timeout(
    "saber-z3-timeout",
    cl::desc("Z3 solver timeout in milliseconds (0 = no timeout)"),
    cl::init(10000u), cl::cat(SaberCategory));

cl::opt<bool> SaberDumpSlice("saber-dump-slice",
                             cl::desc("Dump slice (annotate and dump SVFG)"),
                             cl::init(false), cl::cat(SaberCategory));

cl::opt<bool>
    SaberValidateTests("saber-validate-tests",
                       cl::desc("Run validation tests (for regression)"),
                       cl::init(false), cl::cat(SaberCategory));

cl::opt<bool>
    SaberCollectExtRetGlobals("saber-collect-extret-globals",
                              cl::desc("Collect external-return globals"),
                              cl::init(true), cl::cat(SaberCategory));

cl::opt<bool>
    SaberVerbose("saber-verbose",
                 cl::desc("Enable verbose output (timing, statistics)"),
                 cl::init(false), cl::cat(SaberCategory));

} // namespace analysis
} // namespace lotus
