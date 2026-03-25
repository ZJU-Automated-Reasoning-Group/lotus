#pragma once

#include "llvm/ADT/Optional.h"

#include "Alias/Dynamic/LogRecord.h"

#include <fstream>
#include <vector>

namespace dynamic {

class EagerLogReader {
public:
  EagerLogReader() = delete;

  static std::vector<LogRecord> readLogFromFile(const char *fileName);
};

class LazyLogReader {
private:
  std::ifstream ifs;

public:
  LazyLogReader(const char *fileName);

  llvm::Optional<LogRecord> readLogRecord();
};

} // namespace dynamic
