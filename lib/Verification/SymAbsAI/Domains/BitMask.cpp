#include "Verification/SymAbsAI/Domains/BitMask.h"

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
// #include "Verification/SymAbsAI/Core/ParamStrategy.h"
// #include "Verification/SymAbsAI/Core/repr.h"
// #include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

using namespace domains;
using namespace symabs_ai;
// using std::unique_ptr;

namespace // anonymous
{
/**
 * Returns a value with exactly `bw' lowest bit set.
 */
static uint64_t allOnes(unsigned bw) {
  assert(bw <= 64);

  if (bw == 64)
    return ((uint64_t)0) - 1;
  else
    return (((uint64_t)1) << bw) - 1;
}
} // namespace

void BitMask::assertValid() const {
  // the only allowed representation of bottom is Zeros_ = 0, Ones_ = 1
  if ((~Zeros_ & Ones_) != 0) {
    assert(Zeros_ == 0 && Ones_ == 1);
  } else {
    // all bits in Zeros_ and Ones_ outside the value's bitwidth must be
    // zero
    uint64_t mask = ~allOnes(Bitwidth_);
    (void)mask; // avoid unused variable warning
    assert((Zeros_ & mask) == 0);
    assert((Ones_ & mask) == 0);
  }
}

BitMask::BitMask(const FunctionContext &fctx, RepresentedValue left,
                 RepresentedValue right)
    : FunctionContext_(fctx), Left_(left), Right_(right) {
  assert(left != nullptr);

  Zeros_ = 0;
  Ones_ = 1;

  if (left->getType()->isIntegerTy()) {
    Bitwidth_ = static_cast<int>(left->getType()->getIntegerBitWidth());
  } else if (left->getType()->isPointerTy()) {
    Bitwidth_ = fctx.getPointerSize();
  } else {
    llvm_unreachable("Unsupported type!");
  }

  if (Right_ != nullptr) {
    unsigned int right_size = 0;
    if (right->getType()->isIntegerTy()) {
      right_size = right->getType()->getIntegerBitWidth();
    } else if (right->getType()->isPointerTy()) {
      right_size = fctx.getPointerSize();
    } else {
      llvm_unreachable("Unsupported type!");
    }
    assert(right_size == (unsigned)Bitwidth_);
  }

  assertValid();
}

bool BitMask::joinWith(const AbstractValue &av_other) {
  assertValid();
  auto &other = dynamic_cast<const BitMask &>(av_other);
  bool changed;
  other.assertValid();

  if (isBottom()) {
    Zeros_ = other.Zeros_;
    Ones_ = other.Ones_;
    changed = !other.isBottom();
  } else {
    uint64_t new_zeros = Zeros_ | other.Zeros_;
    uint64_t new_ones = Ones_ & other.Ones_;
    changed = (new_zeros != Zeros_) || (new_ones != Ones_);

    Zeros_ = new_zeros;
    Ones_ = new_ones;
  }

  assertValid();
  return changed;
}

bool BitMask::meetWith(const AbstractValue &av_other) {
  assertValid();
  auto &other = dynamic_cast<const BitMask &>(av_other);
  other.assertValid();

  uint64_t new_zeros = Zeros_ & other.Zeros_;
  uint64_t new_ones = Ones_ | other.Ones_;
  bool changed = (new_zeros != Zeros_) || (new_ones != Ones_);

  Zeros_ = new_zeros;
  Ones_ = new_ones;

  assertValid();
  return changed;
}

bool BitMask::updateWith(const ConcreteState &state) {
  uint64_t val;

  if (Right_ == nullptr)
    val = (uint64_t)state[Left_];
  else
    val = (uint64_t)state[Left_] - (uint64_t)state[Right_];

  val = val & allOnes(Bitwidth_);

  BitMask other(*this);
  other.Zeros_ = val;
  other.Ones_ = val;

  return joinWith(other);
}

z3::expr BitMask::toFormula(const ValueMapping &vmap, z3::context &zctx) const {
  assertValid();

  z3::expr e = Right_ ? vmap[Left_] - vmap[Right_] : vmap[Left_];
  z3::expr ones = zctx.bv_val((uint64_t)Ones_, Bitwidth_);
  z3::expr zeros = zctx.bv_val((uint64_t)Zeros_, Bitwidth_);

  return ((e & ~zeros) | ((~e) & ones)) == zctx.bv_val(0, Bitwidth_);
}

void BitMask::havoc() {
  assertValid();
  Zeros_ = allOnes(Bitwidth_);
  Ones_ = 0;
  assertValid();
}

void BitMask::prettyPrint(PrettyPrinter &out) const {
  assertValid();

  if (isBottom()) {
    out << "bottom";
    return;
  }

  if (Right_ == nullptr)
    out << Left_ << " = ";
  else
    out << Left_ << " = " << Right_ << " + ";

  for (int i = Bitwidth_ - 1; i >= 0; i--) {
    uint64_t mask = ((uint64_t)1) << i;
    assert(mask != 0);
    bool must_zero = (Zeros_ & mask) == 0;
    bool must_one = (Ones_ & mask) != 0;
    assert(!must_zero || !must_one);

    if (must_zero)
      out << "0";
    else if (must_one)
      out << "1";
    else
      out << "*";
  }
}

bool BitMask::isTop() const {
  assertValid();
  uint64_t mask = allOnes(Bitwidth_);
  return (Zeros_ == mask) && (Ones_ == 0);
}

void BitMask::resetToBottom() {
  Zeros_ = 0;
  Ones_ = 1;
  assertValid();
}

bool BitMask::isJoinableWith(const AbstractValue &other) const {
  if (auto *other_val = dynamic_cast<const BitMask *>(&other)) {
    if (other_val->Left_ == Left_ && other_val->Right_ == Right_) {
      return true;
    }
  }
  return false;
}

namespace {
DomainConstructor::Register
    sgl("BitMask/Single", "track possible values of bits of each variable",
        BitMask::NewSingle);

DomainConstructor::Register
    rel("BitMask/Relational",
        "track bit values of a difference between variables",
        BitMask::NewRelational);
} // namespace
