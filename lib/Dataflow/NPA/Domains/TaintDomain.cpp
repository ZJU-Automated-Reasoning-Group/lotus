/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Domains/TaintDomain.h"

namespace npa {

unsigned TaintTransformer::requireBitWidth() {
  return width_context::require(
      "TaintTransformer width must be installed via WidthScope");
}

unsigned TaintTransformer::bitWidthOf(const value_type &value) {
  return value.gen.getBitWidth();
}

std::vector<llvm::APInt> TaintTransformer::identityRel(unsigned bit_width) {
  std::vector<llvm::APInt> rel;
  rel.reserve(bit_width);
  for (unsigned i = 0; i < bit_width; ++i) {
    llvm::APInt row(bit_width, 0);
    row.setBit(i);
    rel.push_back(row);
  }
  return rel;
}

TaintTransformer::value_type TaintTransformer::zero() {
  return zero(requireBitWidth());
}

TaintTransformer::value_type TaintTransformer::zero(unsigned bit_width) {
  value_type out;
  out.rel.assign(bit_width, llvm::APInt(bit_width, 0));
  out.gen = llvm::APInt(bit_width, 0);
  return out;
}

TaintTransformer::value_type TaintTransformer::one() {
  return one(requireBitWidth());
}

TaintTransformer::value_type TaintTransformer::one(unsigned bit_width) {
  value_type out;
  out.rel = identityRel(bit_width);
  out.gen = llvm::APInt(bit_width, 0);
  return out;
}

bool TaintTransformer::equal(const value_type &a, const value_type &b) {
  if (a.gen != b.gen || a.rel.size() != b.rel.size())
    return false;
  for (unsigned i = 0; i < a.rel.size(); ++i) {
    if (a.rel[i] != b.rel[i])
      return false;
  }
  return true;
}

TaintTransformer::value_type
TaintTransformer::combine(const value_type &a, const value_type &b) {
  const unsigned bit_width = bitWidthOf(a);
  assert(bit_width == bitWidthOf(b) && "taint widths must match");
  value_type out;
  out.rel.resize(bit_width, llvm::APInt(bit_width, 0));
  for (unsigned i = 0; i < bit_width; ++i) {
    out.rel[i] = a.rel[i] | b.rel[i];
  }
  out.gen = a.gen | b.gen;
  return out;
}

TaintTransformer::value_type
TaintTransformer::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

TaintTransformer::value_type
TaintTransformer::condCombine(bool phi, const value_type &t,
                              const value_type &e) {
  return phi ? t : e;
}

llvm::APInt TaintTransformer::applyRel(const std::vector<llvm::APInt> &rel,
                                       const llvm::APInt &in) {
  const unsigned bit_width = in.getBitWidth();
  assert(rel.size() == bit_width && "taint relation width must match input");
  llvm::APInt out(bit_width, 0);
  for (unsigned i = 0; i < bit_width; ++i) {
    if (in[i])
      out |= rel[i];
  }
  return out;
}

TaintTransformer::value_type
TaintTransformer::extend(const value_type &a, const value_type &b) {
  // a after b: a o b
  const unsigned bit_width = bitWidthOf(a);
  assert(bit_width == bitWidthOf(b) && "taint widths must match");
  value_type out;
  out.rel.resize(bit_width, llvm::APInt(bit_width, 0));
  for (unsigned i = 0; i < bit_width; ++i) {
    out.rel[i] = applyRel(a.rel, b.rel[i]);
  }
  out.gen = applyRel(a.rel, b.gen) | a.gen;
  return out;
}

TaintTransformer::value_type
TaintTransformer::extend_lin(const value_type &a, const value_type &b) {
  return extend(a, b);
}

TaintTransformer::value_type
TaintTransformer::subtract(const value_type &a, const value_type &b) {
  (void)b;
  return a;
}

llvm::APInt TaintTransformer::apply(const value_type &f,
                                    const llvm::APInt &in) {
  return applyRel(f.rel, in) | f.gen;
}

void TaintTransformer::addEdge(value_type &f, unsigned from, unsigned to) {
  const unsigned bit_width = bitWidthOf(f);
  if (from >= bit_width || to >= bit_width)
    return;
  f.rel[from].setBit(to);
}

void TaintTransformer::addGen(value_type &f, unsigned bit) {
  if (bit >= bitWidthOf(f))
    return;
  f.gen.setBit(bit);
}

} // namespace npa
