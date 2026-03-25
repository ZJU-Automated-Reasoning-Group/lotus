//===-- Verification/Sifa/Log/SifaLogger.cpp ------------------------------===//
//
// Implementation of SifaLogger.
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Log/SifaLogger.h"

#include "llvm/Support/raw_ostream.h"

#include "Verification/Sifa/Statistics/SifaStats.h"

#include <set>
#include <string>

namespace lotus {
namespace sifa {

SifaLogLevel SifaLogger::current_level_ = SifaLogLevel::None;
llvm::raw_ostream *SifaLogger::output_stream_ = &llvm::errs();

void SifaLogger::setLevel(SifaLogLevel level) { current_level_ = level; }

void SifaLogger::setOutputStream(llvm::raw_ostream *os) {
  output_stream_ = os ? os : &llvm::errs();
}

void SifaLogger::log(SifaLogLevel level, const char *prefix,
                     const std::string &msg) {
  if (level > current_level_ || current_level_ == SifaLogLevel::None)
    return;
  if (!output_stream_)
    return;
  *output_stream_ << "[sifa] " << prefix << msg << "\n";
  output_stream_->flush();
}

void SifaLogger::error(const std::string &msg) {
  log(SifaLogLevel::Error, "ERROR: ", msg);
}

void SifaLogger::warning(const std::string &msg) {
  log(SifaLogLevel::Warning, "WARN:  ", msg);
}

void SifaLogger::info(const std::string &msg) {
  log(SifaLogLevel::Info, "INFO:  ", msg);
}

void SifaLogger::progress(const std::string &msg) {
  log(SifaLogLevel::Progress, "", msg);
}

void SifaLogger::debug(const std::string &msg) {
  log(SifaLogLevel::Debug, "DEBUG: ", msg);
}

void SifaLogger::trace(const std::string &msg) {
  log(SifaLogLevel::Trace, "TRACE: ", msg);
}

std::string SifaLogger::formatLocation(const llvm::Function *F) {
  if (!F)
    return "<unknown>";
  std::string name = F->getName().str();
  return name.empty() ? "<unnamed>" : name;
}

std::string SifaLogger::formatLocation(const llvm::BasicBlock *BB) {
  if (!BB)
    return "<unknown>";
  std::string name = BB->getName().str();
  return name.empty() ? "(unnamed)" : name;
}

void SifaLogger::logProcedure(const llvm::Function *F,
                              const std::string &action) {
  if (current_level_ < SifaLogLevel::Debug)
    return;
  info("Procedure " + formatLocation(F) + ": " + action);
}

void SifaLogger::logBlock(const llvm::BasicBlock *BB,
                          const std::string &action) {
  if (current_level_ < SifaLogLevel::Trace)
    return;
  trace("Block " + formatLocation(BB) + ": " + action);
}

void SifaLogger::printStats(const SifaStats &stats) {
  if (!output_stream_)
    return;
  llvm::raw_ostream &OS = *output_stream_;
  OS << "[sifa] Statistics:\n";

  const auto keys = stats.getKeys();
  const auto stopwatches = stats.getStopwatches();
  std::set<std::string> stopwatchSet(stopwatches.begin(), stopwatches.end());

  for (const std::string &key : keys) {
    std::uint64_t val = stats.getValue(key);
    if (val == 0)
      continue;
    OS << "  " << key << ": ";
    if (stopwatchSet.count(key)) {
      // Nanoseconds to milliseconds for readability
      double ms = static_cast<double>(val) / 1e6;
      OS << ms << " ms";
    } else {
      OS << val;
    }
    OS << "\n";
  }
  output_stream_->flush();
}

} // namespace sifa
} // namespace lotus
