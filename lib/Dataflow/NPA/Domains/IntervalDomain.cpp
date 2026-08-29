#include "Dataflow/NPA/Domains/IntervalDomain.h"

namespace npa {
namespace {

bool apIntLess(const llvm::APInt &Lhs, const llvm::APInt &Rhs) {
  if (Lhs.getBitWidth() != Rhs.getBitWidth())
    return Lhs.getBitWidth() < Rhs.getBitWidth();
  return Lhs.ult(Rhs);
}

bool apIntEqual(const llvm::APInt &Lhs, const llvm::APInt &Rhs) {
  return Lhs.getBitWidth() == Rhs.getBitWidth() && Lhs.eq(Rhs);
}

} // namespace

bool IntervalOp::operator<(const IntervalOp &Other) const {
  if (kind != Other.kind)
    return kind < Other.kind;
  if (dest != Other.dest)
    return dest < Other.dest;
  if (lhs != Other.lhs)
    return lhs < Other.lhs;
  if (rhs != Other.rhs)
    return rhs < Other.rhs;
  if (cond != Other.cond)
    return cond < Other.cond;
  if (opcode != Other.opcode)
    return opcode < Other.opcode;
  if (bitWidth != Other.bitWidth)
    return bitWidth < Other.bitWidth;
  if (sourceBitWidth != Other.sourceBitWidth)
    return sourceBitWidth < Other.sourceBitWidth;
  if (ordering != Other.ordering)
    return ordering < Other.ordering;
  if (!apIntEqual(constant, Other.constant))
    return apIntLess(constant, Other.constant);
  return inputs < Other.inputs;
}

bool IntervalOp::operator==(const IntervalOp &Other) const {
  return kind == Other.kind && dest == Other.dest && lhs == Other.lhs &&
         rhs == Other.rhs && cond == Other.cond && opcode == Other.opcode &&
         bitWidth == Other.bitWidth && sourceBitWidth == Other.sourceBitWidth &&
         ordering == Other.ordering && apIntEqual(constant, Other.constant) &&
         inputs == Other.inputs;
}

bool IntervalOp::summaryCanBeOverwritten() const {
  switch (kind) {
  case Kind::AssignConst:
  case Kind::Copy:
  case Kind::Cast:
  case Kind::Binary:
  case Kind::Compare:
  case Kind::Forget:
    return true;
  case Kind::AssumeNotCases:
  case Kind::Select:
  case Kind::Phi:
    return false;
  }
  return false;
}

bool IntervalOp::summaryCanOverwritePrevious() const {
  return summaryCanBeOverwritten();
}

} // namespace npa
