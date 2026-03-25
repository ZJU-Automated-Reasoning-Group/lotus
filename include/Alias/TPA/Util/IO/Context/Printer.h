#pragma once

#include <llvm/Support/raw_ostream.h>

namespace context {
class Context;
class ProgramPoint;
} // namespace context

namespace util {
namespace io {

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const context::Context &c);
llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
                              const context::ProgramPoint &p);

} // namespace io
} // namespace util