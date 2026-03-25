//===- SaberOptions.h -- Saber checker options ----------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// Uses LLVM cl::opt for command-line options.
//
//===----------------------------------------------------------------------===//

#ifndef SABER_OPTIONS_H
#define SABER_OPTIONS_H

#include <llvm/Support/CommandLine.h>

namespace lotus {
namespace analysis {

// Forward declarations - defined in SaberOptions.cpp
extern llvm::cl::opt<bool> SaberFullSVFG;
extern llvm::cl::opt<unsigned> SaberCxtLimit;
extern llvm::cl::opt<unsigned> SaberMaxStepInWrapper;
extern llvm::cl::opt<unsigned> SaberMaxForwardItems;
extern llvm::cl::opt<unsigned> SaberZ3Timeout;
extern llvm::cl::opt<bool> SaberDumpSlice;
extern llvm::cl::opt<bool> SaberValidateTests;
extern llvm::cl::opt<bool> SaberCollectExtRetGlobals;
extern llvm::cl::opt<bool> SaberVerbose;

/// Saber-related options wrapper (for backward compatibility).
struct SaberOptions {
  /// Use the full, unoptimized SABER SVFG. Default false uses the optimized
  /// compatibility graph.
  static bool fullSVFG() { return SaberFullSVFG; }

  /// Max call-string context length (k-limit). Default 5.
  static unsigned cxtLimit() { return SaberCxtLimit; }

  /// Max steps in wrapper detection. Default 100.
  static unsigned maxStepInWrapper() { return SaberMaxStepInWrapper; }

  /// Max (node, context) items per source in forward traversal (0 = no limit).
  /// Safety cap only.
  static unsigned maxForwardItems() { return SaberMaxForwardItems; }

  /// Z3 solver timeout in milliseconds (0 = no timeout). Default 10000.
  static unsigned z3Timeout() { return SaberZ3Timeout; }

  /// Dump slice (annotate and dump SVFG when set). Default false.
  static bool dumpSlice() { return SaberDumpSlice; }

  /// Run validation tests (for regression). Default false.
  static bool validateTests() { return SaberValidateTests; }

  /// Collect external-return globals (SVF saber-collect-extret-globals).
  /// Default true.
  static bool collectExtRetGlobals() { return SaberCollectExtRetGlobals; }

  /// Enable verbose output (timing, statistics). Default false.
  static bool verbose() { return SaberVerbose; }
};

} // namespace analysis
} // namespace lotus

#endif
