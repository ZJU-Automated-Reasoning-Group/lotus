//===- SaberOptions.cpp -- Saber checker options -------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// Command-line option definitions.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SaberOptions.h"

#include "Checker/Framework/Subcommands.h"

#include <llvm/Support/CommandLine.h>

using namespace llvm;

namespace lotus {
namespace analysis {

// Define option category
static cl::OptionCategory
    SaberCategory("Saber Options", "Options for Saber source-sink checker");

// Define options
cl::opt<bool>
    SaberFullSVFG("saber.full-svfg",
                  cl::desc("Use the full, unoptimized SABER SVFG "
                           "(default uses the optimized "
                           "compatibility graph)"),
                  cl::init(false), cl::cat(SaberCategory),
                  cl::sub(lotus::checker::tooling::saberSubCommand()));

cl::opt<unsigned>
    SaberCxtLimit("saber.context-limit",
                  cl::desc("Max call-string context length (k-limit; beyond "
                           "this contexts merge to avoid explosion)"),
                  cl::init(3u), cl::cat(SaberCategory),
                  cl::sub(lotus::checker::tooling::saberSubCommand()));

cl::opt<unsigned>
    SaberMaxStepInWrapper("saber.max-steps-in-wrapper",
                          cl::desc("Max steps in wrapper detection"),
                          cl::init(10u), cl::cat(SaberCategory),
                          cl::sub(lotus::checker::tooling::saberSubCommand()));

cl::opt<unsigned> SaberMaxForwardItems(
    "saber.max-forward-items",
    cl::desc("Max (node,context) items per source in forward traversal (0 = no "
             "limit); safety cap only"),
    cl::init(0u), cl::cat(SaberCategory),
    cl::sub(lotus::checker::tooling::saberSubCommand()));

cl::opt<unsigned> SaberZ3Timeout(
    "saber.solver-timeout-ms",
    cl::desc("Z3 solver timeout in milliseconds (0 = no timeout)"),
    cl::init(10000u), cl::cat(SaberCategory),
    cl::sub(lotus::checker::tooling::saberSubCommand()));

cl::opt<bool>
    SaberDumpSlice("saber.dump-slice",
                   cl::desc("Dump slice (annotate and dump SVFG)"),
                   cl::init(false), cl::cat(SaberCategory),
                   cl::sub(lotus::checker::tooling::saberSubCommand()));

bool SaberValidateTests = false;

cl::opt<bool> SaberCollectExtRetGlobals(
    "saber.collect-external-return-globals",
    cl::desc("Collect external-return globals"), cl::init(true),
    cl::cat(SaberCategory),
    cl::sub(lotus::checker::tooling::saberSubCommand()));

bool SaberVerbose = false;

} // namespace analysis
} // namespace lotus
