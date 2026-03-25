#pragma once

#include "Alias/TPA/Util/Hashing.h"

namespace llvm {
class Instruction;
} // namespace llvm

namespace context {

class Context;

class ProgramPoint {
private:
  const Context *context;
  const llvm::Instruction *inst;

  using PairType = std::pair<const Context *, const llvm::Instruction *>;

public:
  ProgramPoint(const Context *c, const llvm::Instruction *i)
      : context(c), inst(i) {}
  ProgramPoint(const PairType &p) : context(p.first), inst(p.second) {}

  const Context *getContext() const { return context; }
  const llvm::Instruction *getInstruction() const { return inst; }

  bool operator==(const ProgramPoint &other) const {
    return (context == other.context) && (inst == other.inst);
  }
  bool operator!=(const ProgramPoint &other) const { return !(*this == other); }
  bool operator<(const ProgramPoint &rhs) const {
    if (context < rhs.context)
      return true;
    else if (rhs.context < context)
      return false;
    else
      return inst < rhs.inst;
  }
  bool operator>(const ProgramPoint &rhs) const { return rhs < *this; }
  bool operator<=(const ProgramPoint &rhs) const { return !(rhs < *this); }
  bool operator>=(const ProgramPoint &rhs) const { return !(*this < rhs); }

  operator PairType() const { return std::make_pair(context, inst); }
};

} // namespace context

namespace std {
template <> struct hash<context::ProgramPoint> {
  size_t operator()(const context::ProgramPoint &pp) const {
    return util::hashPair(pp.getContext(), pp.getInstruction());
  }
};
} // namespace std
