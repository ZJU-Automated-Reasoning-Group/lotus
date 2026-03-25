#include "Alias/Dynamic/LogReader.h"

#include <cassert>
#include <iostream>

namespace dynamic {

/// Reads a binary value of type T from the input stream
template <typename T> static bool readData(std::istream &is, T *data) {
  auto *charPtr = reinterpret_cast<char *>(data);
  is.read(charPtr, sizeof(T));
  return is.good();
}

/// Reads a single log record from the binary stream based on its type tag
static llvm::Optional<LogRecord> readRecord(std::istream &is) {
  LogRecord rec;

  bool succ = true;
  char type;
  succ &= readData(is, &type);
  switch (type) {
  case TAllocRec:
    succ &= readData(is, &rec.allocRecord.type);
    succ &= readData(is, &rec.allocRecord.id);
    succ &= readData(is, &rec.allocRecord.address);
    break;
  case TPointerRec:
    succ &= readData(is, &rec.ptrRecord.id);
    succ &= readData(is, &rec.ptrRecord.address);
    break;
  case TEnterRec:
    succ &= readData(is, &rec.enterRecord.id);
    break;
  case TExitRec:
    succ &= readData(is, &rec.exitRecord.id);
    break;
  case TCallRec:
    succ &= readData(is, &rec.callRecord.id);
    break;
  default: {
    std::cerr << static_cast<unsigned>(rec.type) << "\n";
    std::cerr << "Illegal record type. Log file must be broken.\n";
    std::exit(-1);
  }
  }
  rec.type = static_cast<LogRecordType>(type);

  if (!succ)
    return llvm::None;
  return llvm::Optional<LogRecord>(std::move(rec));
}

/// Reads all log records from a file into memory (eager loading)
std::vector<LogRecord> EagerLogReader::readLogFromFile(const char *fileName) {
  std::vector<LogRecord> ret;

  std::ifstream file(fileName, std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
    auto size = file.tellg();
    auto numEntry = size / sizeof(LogRecord);
    ret.reserve(numEntry);

    file.seekg(0, std::ios::beg);
    for (auto i = 0u; i < static_cast<size_t>(numEntry); ++i) {
      auto rec = readRecord(file);
      assert(rec && "Log read failure");
      ret.emplace_back(std::move(*rec));
    }
  }

  return ret;
}

/// Opens a log file for lazy (streaming) reading
LazyLogReader::LazyLogReader(const char *fileName)
    : ifs(fileName, std::ios::in | std::ios::binary) {
  if (!ifs.is_open()) {
    std::cerr << "Open log file " << fileName << " failed\n";
    std::exit(-1);
  }
}

/// Reads the next log record from the file (lazy loading, one at a time)
llvm::Optional<LogRecord> LazyLogReader::readLogRecord() {
  return readRecord(ifs);
}

} // namespace dynamic
