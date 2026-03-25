//===-- Verification/Sifa/Statistics/SifaLogger.h -------------------------===//
//
// Lightweight logger for Sifa. Wraps llvm::errs() and provides
// level-filtered output.
//
// Thread-safety: the global singleton's level_ is protected by a mutex so
// that concurrent analyses (e.g. parallel LLVM passes) do not race on reads
// and writes. Individual log() calls are serialised by the same mutex.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STATISTICS_SIFALOGGER_H
#define LOTUS_VERIFICATION_SIFA_STATISTICS_SIFALOGGER_H

#include "llvm/Support/raw_ostream.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace lotus {
namespace sifa {

class SifaLogger {
public:
  enum class Level { Off = 0, Error, Warn, Info, Debug, Trace };

  static SifaLogger &instance() {
    static SifaLogger inst;
    return inst;
  }

  void setLevel(Level l) {
    std::lock_guard<std::mutex> lk(mu_);
    level_ = l;
  }
  Level getLevel() const {
    std::lock_guard<std::mutex> lk(mu_);
    return level_;
  }

  template <typename... Args>
  void log(Level l, const char *fmt, Args &&...args) {
    std::lock_guard<std::mutex> lk(mu_);
    if (l > level_)
      return;
    char buf[512];
    std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    llvm::errs() << "[sifa] " << buf << "\n";
  }

  void error(const std::string &msg) { log(Level::Error, "%s", msg.c_str()); }
  void warn(const std::string &msg) { log(Level::Warn, "%s", msg.c_str()); }
  void info(const std::string &msg) { log(Level::Info, "%s", msg.c_str()); }
  void debug(const std::string &msg) { log(Level::Debug, "%s", msg.c_str()); }
  void trace(const std::string &msg) { log(Level::Trace, "%s", msg.c_str()); }

private:
  SifaLogger() = default;
  mutable std::mutex mu_;
  Level level_ = Level::Warn;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_STATISTICS_SIFALOGGER_H
