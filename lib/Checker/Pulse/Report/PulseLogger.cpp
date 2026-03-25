#include "Checker/Pulse/Report/PulseLogger.h"

#include "Checker/Pulse/Domain/PulseExecutionDomain.h"

#include <iomanip>
#include <sstream>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

LogLevel PulseLogger::current_level_ = LogLevel::Info;
llvm::raw_ostream *PulseLogger::output_stream_ = &llvm::errs();
std::map<std::string, std::chrono::steady_clock::time_point>
    PulseLogger::timers_;
std::map<std::string, unsigned> PulseLogger::counters_;

void PulseLogger::setLevel(LogLevel level) { current_level_ = level; }

void PulseLogger::setOutputStream(llvm::raw_ostream *os) {
  output_stream_ = os;
}

void PulseLogger::log(LogLevel level, const std::string &prefix,
                      const std::string &msg) {
  if (level > current_level_ || current_level_ == LogLevel::None)
    return;

  *output_stream_ << "[Pulse] " << prefix << msg << "\n";
  output_stream_->flush();
}

void PulseLogger::error(const std::string &msg) {
  log(LogLevel::Error, "ERROR: ", msg);
}

void PulseLogger::warning(const std::string &msg) {
  log(LogLevel::Warning, "WARN:  ", msg);
}

void PulseLogger::info(const std::string &msg) {
  log(LogLevel::Info, "INFO:  ", msg);
}

void PulseLogger::debug(const std::string &msg) {
  log(LogLevel::Debug, "DEBUG: ", msg);
}

void PulseLogger::trace(const std::string &msg) {
  log(LogLevel::Trace, "TRACE: ", msg);
}

std::string PulseLogger::formatLocation(const llvm::Function *F) {
  if (!F)
    return "<unknown>";
  std::string name = F->getName().str();
  if (name.empty())
    name = "<unnamed>";
  return name;
}

std::string PulseLogger::formatLocation(const llvm::BasicBlock *BB) {
  if (!BB)
    return "<unknown>";
  std::string name = BB->getName().str();
  if (name.empty()) {
    std::ostringstream oss;
    oss << "BB#"
        << std::distance(BB->getParent()->begin(),
                         llvm::Function::const_iterator(BB));
    name = oss.str();
  }
  return formatLocation(BB->getParent()) + "::" + name;
}

std::string PulseLogger::formatLocation(const llvm::Instruction *I) {
  if (!I)
    return "<unknown>";
  std::ostringstream oss;
  oss << formatLocation(I->getParent()) << ":";
  if (I->hasName()) {
    oss << I->getName().str();
  } else {
    oss << I->getOpcodeName();
  }
  return oss.str();
}

void PulseLogger::logFunction(const llvm::Function *F,
                              const std::string &action) {
  if (current_level_ >= LogLevel::Info) {
    std::ostringstream oss;
    oss << "Function " << formatLocation(F) << ": " << action;
    info(oss.str());
  }
}

void PulseLogger::logBlock(const llvm::BasicBlock *BB,
                           const std::string &action) {
  if (current_level_ >= LogLevel::Debug) {
    std::ostringstream oss;
    oss << "Block " << formatLocation(BB) << ": " << action;
    debug(oss.str());
  }
}

void PulseLogger::logInstruction(const llvm::Instruction *I,
                                 const std::string &action) {
  if (current_level_ >= LogLevel::Trace) {
    std::ostringstream oss;
    oss << "Instruction " << formatLocation(I) << ": " << action;
    trace(oss.str());
  }
}

void PulseLogger::logBug(OperationResult kind, const llvm::Instruction *loc,
                         const std::string &details) {
  std::ostringstream oss;
  oss << "BUG DETECTED: ";

  switch (kind) {
  case OperationResult::UseAfterFree:
    oss << "UseAfterFree";
    break;
  case OperationResult::OutOfBounds:
    oss << "OutOfBounds";
    break;
  case OperationResult::NullDereference:
    oss << "NullDereference";
    break;
  case OperationResult::UninitializedRead:
    oss << "UninitializedRead";
    break;
  case OperationResult::TaintError:
    oss << "TaintError";
    break;
  default:
    oss << "Unknown";
    break;
  }

  oss << " at " << formatLocation(loc);
  if (!details.empty()) {
    oss << " (" << details << ")";
  }

  error(oss.str());
  incrementCounter(
      "bugs." +
      std::string(kind == OperationResult::UseAfterFree      ? "use_after_free"
                  : kind == OperationResult::OutOfBounds     ? "out_of_bounds"
                  : kind == OperationResult::NullDereference ? "null_deref"
                  : kind == OperationResult::UninitializedRead ? "uninit_read"
                  : kind == OperationResult::TaintError        ? "taint_error"
                                                               : "unknown"));
}

void PulseLogger::startTimer(const std::string &name) {
  timers_[name] = std::chrono::steady_clock::now();
}

void PulseLogger::endTimer(const std::string &name) {
  auto it = timers_.find(name);
  if (it != timers_.end()) {
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - it->second)
            .count();

    if (current_level_ >= LogLevel::Debug) {
      std::ostringstream oss;
      oss << "Timer '" << name << "': " << duration << "ms";
      debug(oss.str());
    }

    timers_.erase(it);
  }
}

void PulseLogger::incrementCounter(const std::string &name, unsigned delta) {
  counters_[name] += delta;
}

unsigned PulseLogger::getCounter(const std::string &name) {
  auto it = counters_.find(name);
  return (it != counters_.end()) ? it->second : 0;
}

void PulseLogger::printStats() {
  if (current_level_ >= LogLevel::Info) {
    info("=== Pulse Analysis Statistics ===");

    // Bug counts
    unsigned use_after_free = getCounter("bugs.use_after_free");
    unsigned null_deref = getCounter("bugs.null_deref");
    unsigned uninit_read = getCounter("bugs.uninit_read");
    unsigned taint_error = getCounter("bugs.taint_error");
    unsigned total_bugs =
        use_after_free + null_deref + uninit_read + taint_error;

    if (total_bugs > 0) {
      std::ostringstream oss;
      oss << "Bugs found: " << total_bugs
          << " (UseAfterFree: " << use_after_free
          << ", NullDeref: " << null_deref << ", UninitRead: " << uninit_read
          << ", TaintError: " << taint_error << ")";
      info(oss.str());
    }

    // Analysis statistics
    unsigned functions_analyzed = getCounter("functions.analyzed");
    unsigned summaries_created = getCounter("summaries.created");
    unsigned summaries_applied = getCounter("summaries.applied");
    unsigned joins_performed = getCounter("joins.performed");
    unsigned paths_explored = getCounter("paths.explored");

    std::ostringstream oss2;
    oss2 << "Analysis: " << functions_analyzed << " functions analyzed, "
         << summaries_created << " summaries created, " << summaries_applied
         << " summaries applied, " << joins_performed << " joins performed, "
         << paths_explored << " paths explored";
    info(oss2.str());

    info("=================================");
  }
}

void PulseLogger::resetStats() {
  counters_.clear();
  timers_.clear();
}

} // namespace pulse
