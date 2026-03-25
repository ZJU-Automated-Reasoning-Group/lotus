#pragma once

#include <memory>

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>

namespace util {
namespace io {

inline std::unique_ptr<llvm::MemoryBuffer>
readFileIntoBuffer(const char *fileName) {
  auto fileOrErr = llvm::MemoryBuffer::getFile(fileName);
  if (auto ec = fileOrErr.getError()) {
    auto errMsg =
        llvm::Twine("Can't open file \'") + fileName + "\' :" + ec.message();
    llvm::report_fatal_error(errMsg);
  }

  return std::move(fileOrErr.get());
}

} // namespace io
} // namespace util
