#pragma once

#include "Alias/TPA/Util/Hashing.h"

#include <cassert>

namespace context {
class Context;
} // namespace context

namespace llvm {
class Function;
} // namespace llvm

namespace tpa {

class FunctionContext {
private:
  const context::Context *ctx;
  const llvm::Function *func;

public:
  FunctionContext(const context::Context *c, const llvm::Function *f)
      : ctx(c), func(f) {
    assert(ctx != nullptr && func != nullptr);
  }

  const context::Context *getContext() const { return ctx; }
  const llvm::Function *getFunction() const { return func; }

  bool operator==(const FunctionContext &rhs) const {
    return ctx == rhs.ctx && func == rhs.func;
  }
  bool operator!=(const FunctionContext &rhs) const { return !(*this == rhs); }
  bool operator<(const FunctionContext &rhs) const {
    if (ctx < rhs.ctx)
      return true;
    else if (rhs.ctx < ctx)
      return false;
    else
      return func < rhs.func;
  }
  bool operator>(const FunctionContext &rhs) const { return rhs < *this; }
  bool operator<=(const FunctionContext &rhs) const { return !(rhs < *this); }
  bool operator>=(const FunctionContext &rhs) const { return !(*this < rhs); }
};

} // namespace tpa

namespace std {
template <> struct hash<tpa::FunctionContext> {
  size_t operator()(const tpa::FunctionContext &f) const {
    return util::hashPair(f.getContext(), f.getFunction());
  }
};
} // namespace std
