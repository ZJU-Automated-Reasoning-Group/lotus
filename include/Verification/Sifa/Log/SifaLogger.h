//===-- Verification/Sifa/Log/SifaLogger.h --------------------------------===//
//
// Configurable logging for Sifa (Symbolic Interpretation with Fluid
// Abstractions). Provides log levels, output stream control, and integration
// with SifaStats.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_LOG_SIFALOGGER_H
#define LOTUS_VERIFICATION_SIFA_LOG_SIFALOGGER_H

#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace sifa {

class SifaStats;

/// Log levels for Sifa (aligned with Pulse-style verbosity).
enum class SifaLogLevel {
  None = 0,
  Error = 1,
  Warning = 2,
  Info = 3,
  Progress = 4, /// Progress messages (e.g., "Building module context...")
  Debug = 5,
  Trace = 6,
};

/// Logger for Sifa with configurable verbosity and output.
class SifaLogger {
public:
  static void setLevel(SifaLogLevel level);
  static void setOutputStream(llvm::raw_ostream *os);
  static SifaLogLevel getLevel() { return current_level_; }
  static llvm::raw_ostream *getOutputStream() { return output_stream_; }

  /// Level-checked logging. Messages are prefixed with [sifa] and level tag.
  static void error(const std::string &msg);
  static void warning(const std::string &msg);
  static void info(const std::string &msg);
  static void progress(const std::string &msg);
  static void debug(const std::string &msg);
  static void trace(const std::string &msg);

  /// Convenience: log if level is enabled.
  static bool isEnabled(SifaLogLevel level) {
    return level != SifaLogLevel::None && level <= current_level_;
  }

  /// Contextual logging with LLVM IR.
  static void logProcedure(const llvm::Function *F, const std::string &action);
  static void logBlock(const llvm::BasicBlock *BB, const std::string &action);

  /// Write SifaStats summary to the current output stream.
  static void printStats(const SifaStats &stats);

private:
  static void log(SifaLogLevel level, const char *prefix,
                  const std::string &msg);
  static std::string formatLocation(const llvm::Function *F);
  static std::string formatLocation(const llvm::BasicBlock *BB);

  static SifaLogLevel current_level_;
  static llvm::raw_ostream *output_stream_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_LOG_SIFALOGGER_H
