/*
 * Taint Analysis Implementation
 *
 * Author: rainoftime
 */

#include "Dataflow/IFDS/Clients/IFDSTaintAnalysis.h"

#include <ostream>

#include <llvm/IR/GlobalVariable.h>

namespace ifds {

// ============================================================================
// TaintFact Implementation
// ============================================================================

TaintFact::TaintFact()
    : m_type(ZERO), m_value(nullptr), m_memory_location(nullptr),
      m_source_inst(nullptr), m_field_index(-1), m_taint_kind(TAINT_UNKNOWN) {}

TaintFact TaintFact::zero() { return TaintFact(); }

TaintFact TaintFact::tainted_var(const llvm::Value *v,
                                 const llvm::Instruction *source) {
  TaintFact fact;
  fact.m_type = TAINTED_VAR;
  fact.m_value = v;
  fact.m_source_inst = source;
  return fact;
}

TaintFact TaintFact::tainted_memory(const llvm::Value *loc,
                                    const llvm::Instruction *source) {
  TaintFact fact;
  fact.m_type = TAINTED_MEMORY;
  fact.m_memory_location = loc;
  fact.m_source_inst = source;
  return fact;
}

TaintFact TaintFact::tainted_field(const llvm::Value *base, int field_idx,
                                   const llvm::Instruction *source) {
  TaintFact fact;
  fact.m_type = TAINTED_FIELD;
  fact.m_value = base;
  fact.m_field_index = field_idx;
  fact.m_source_inst = source;
  return fact;
}

TaintFact TaintFact::tainted_global(const llvm::GlobalVariable *gv,
                                    const llvm::Instruction *source) {
  TaintFact fact;
  fact.m_type = TAINTED_GLOBAL;
  fact.m_value = gv;
  fact.m_source_inst = source;
  return fact;
}

TaintFact TaintFact::tainted_implicit(const llvm::Value *control_val,
                                      const llvm::Instruction *source) {
  TaintFact fact;
  fact.m_type = TAINTED_IMPLICIT;
  fact.m_value = control_val;
  fact.m_source_inst = source;
  return fact;
}

bool TaintFact::operator==(const TaintFact &other) const {
  if (m_type != other.m_type)
    return false;
  switch (m_type) {
  case ZERO:
    return true;
  case TAINTED_VAR:
    return m_value == other.m_value;
  case TAINTED_MEMORY:
    return m_memory_location == other.m_memory_location;
  case TAINTED_FIELD:
    return m_value == other.m_value && m_field_index == other.m_field_index;
  case TAINTED_GLOBAL:
  case TAINTED_IMPLICIT:
    return m_value == other.m_value;
  }
  return false;
}

bool TaintFact::operator<(const TaintFact &other) const {
  if (m_type != other.m_type)
    return m_type < other.m_type;
  switch (m_type) {
  case ZERO:
    return false;
  case TAINTED_VAR:
    return m_value < other.m_value;
  case TAINTED_MEMORY:
    return m_memory_location < other.m_memory_location;
  case TAINTED_FIELD:
    if (m_value != other.m_value)
      return m_value < other.m_value;
    return m_field_index < other.m_field_index;
  case TAINTED_GLOBAL:
  case TAINTED_IMPLICIT:
    return m_value < other.m_value;
  }
  return false;
}

bool TaintFact::operator!=(const TaintFact &other) const {
  return !(*this == other);
}

TaintFact::Type TaintFact::get_type() const { return m_type; }

const llvm::Value *TaintFact::get_value() const { return m_value; }

const llvm::Value *TaintFact::get_memory_location() const {
  return m_memory_location;
}

const llvm::Instruction *TaintFact::get_source() const { return m_source_inst; }

int TaintFact::get_field_index() const { return m_field_index; }

TaintFact::TaintKind TaintFact::get_taint_kind() const { return m_taint_kind; }

TaintFact TaintFact::with_source(const llvm::Instruction *source) const {
  TaintFact fact = *this;
  if (!fact.m_source_inst) {
    fact.m_source_inst = source;
  }
  return fact;
}

TaintFact TaintFact::with_kind(TaintKind kind) const {
  TaintFact fact = *this;
  fact.m_taint_kind = kind;
  return fact;
}

bool TaintFact::is_zero() const { return m_type == ZERO; }

bool TaintFact::is_tainted_var() const { return m_type == TAINTED_VAR; }

bool TaintFact::is_tainted_memory() const { return m_type == TAINTED_MEMORY; }

bool TaintFact::is_tainted_field() const { return m_type == TAINTED_FIELD; }

bool TaintFact::is_tainted_global() const { return m_type == TAINTED_GLOBAL; }

bool TaintFact::is_tainted_implicit() const {
  return m_type == TAINTED_IMPLICIT;
}

std::ostream &operator<<(std::ostream &os, const TaintFact &fact) {
  switch (fact.m_type) {
  case TaintFact::ZERO:
    os << "ZERO";
    break;
  case TaintFact::TAINTED_VAR:
    os << "Tainted(" << fact.m_value->getName().str() << ")";
    break;
  case TaintFact::TAINTED_MEMORY:
    os << "TaintedMem(" << fact.m_memory_location->getName().str() << ")";
    break;
  case TaintFact::TAINTED_FIELD:
    os << "TaintedField(" << fact.m_value->getName().str() << "["
       << fact.m_field_index << "])";
    break;
  case TaintFact::TAINTED_GLOBAL:
    os << "TaintedGlobal(" << fact.m_value->getName().str() << ")";
    break;
  case TaintFact::TAINTED_IMPLICIT:
    os << "TaintedImplicit(" << fact.m_value->getName().str() << ")";
    break;
  }
  return os;
}

} // namespace ifds

// Hash function implementation
namespace std {
size_t hash<ifds::TaintFact>::operator()(const ifds::TaintFact &fact) const {
  // Use FNV-1a-style mixing to avoid the collision problems of XOR-shifting
  // small integers and aligned pointers by 1/2 bits.
  size_t h = 14695981039346656037ULL;
  auto mix = [&h](size_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  mix(std::hash<int>{}(static_cast<int>(fact.get_type())));
  if (fact.get_value()) {
    mix(std::hash<const llvm::Value *>{}(fact.get_value()));
  } else if (fact.get_memory_location()) {
    mix(std::hash<const llvm::Value *>{}(fact.get_memory_location()));
  }
  // Include field index in hash for field-sensitive facts
  mix(std::hash<int>{}(fact.get_field_index()));
  return h;
}
} // namespace std
