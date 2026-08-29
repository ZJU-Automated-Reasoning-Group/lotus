#include "Dataflow/NPA/Domains/ConstantPropagationDomain.h"

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

bool ConstantPropagationOp::operator<(
    const ConstantPropagationOp &Other) const {
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
  if (!apIntEqual(constant, Other.constant))
    return apIntLess(constant, Other.constant);
  return inputs < Other.inputs;
}

bool ConstantPropagationOp::operator==(
    const ConstantPropagationOp &Other) const {
  return kind == Other.kind && dest == Other.dest && lhs == Other.lhs &&
         rhs == Other.rhs && cond == Other.cond && opcode == Other.opcode &&
         bitWidth == Other.bitWidth && sourceBitWidth == Other.sourceBitWidth &&
         apIntEqual(constant, Other.constant) && inputs == Other.inputs;
}

bool ConstantPropagationOp::summaryCanBeOverwritten() const {
  switch (kind) {
  case Kind::AssignConst:
  case Kind::Copy:
  case Kind::Cast:
  case Kind::Binary:
  case Kind::Compare:
  case Kind::Forget:
    return true;
  case Kind::AssumeNotCases:
  case Kind::Phi:
  case Kind::Select:
    return false;
  }
  return false;
}

bool ConstantPropagationOp::summaryCanOverwritePrevious() const {
  return summaryCanBeOverwritten();
}

} // namespace npa
