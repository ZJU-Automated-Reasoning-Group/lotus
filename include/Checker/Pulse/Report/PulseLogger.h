#ifndef CHECKER_PULSE_PULSELOGGER_H
#define CHECKER_PULSE_PULSELOGGER_H

#include <chrono>
#include <map>
#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

// Forward declaration - OperationResult is defined in PulseExecutionDomain.h
enum class OperationResult;

/**
 * Log levels for Pulse checker
 */
enum class LogLevel {
  None = 0,
  Error = 1,
  Warning = 2,
  Info = 3,
  Debug = 4,
  Trace = 5
};

/**
 * Logger for Pulse checker with configurable verbosity
 */
class PulseLogger {
private:
  static LogLevel current_level_;
  static llvm::raw_ostream *output_stream_;
  static std::map<std::string, std::chrono::steady_clock::time_point> timers_;
  static std::map<std::string, unsigned> counters_;

public:
  static void setLevel(LogLevel level);
  static void setOutputStream(llvm::raw_ostream *os);
  static LogLevel getLevel() { return current_level_; }

  // Logging methods
  static void error(const std::string &msg);
  static void warning(const std::string &msg);
  static void info(const std::string &msg);
  static void debug(const std::string &msg);
  static void trace(const std::string &msg);

  // Contextual logging
  static void logFunction(const llvm::Function *F, const std::string &action);
  static void logBlock(const llvm::BasicBlock *BB, const std::string &action);
  static void logInstruction(const llvm::Instruction *I,
                             const std::string &action);
  static void logBug(OperationResult kind, const llvm::Instruction *loc,
                     const std::string &details = "");

  // Performance tracking
  static void startTimer(const std::string &name);
  static void endTimer(const std::string &name);
  static void incrementCounter(const std::string &name, unsigned delta = 1);
  static unsigned getCounter(const std::string &name);

  // Statistics
  static void printStats();
  static void resetStats();

private:
  static void log(LogLevel level, const std::string &prefix,
                  const std::string &msg);
  static std::string formatLocation(const llvm::Instruction *I);
  static std::string formatLocation(const llvm::BasicBlock *BB);
  static std::string formatLocation(const llvm::Function *F);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSELOGGER_H
