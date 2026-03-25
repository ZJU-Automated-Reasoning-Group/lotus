//===-- Verification/Sifa/Cfg/Transition.cpp ------------------------------===//
//
// Non-inline definitions for Transition (ostream, etc.).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <utility>

namespace lotus {
namespace sifa {

namespace {

template <typename T> void combineHash(std::size_t &seed, const T &value) {
  const std::size_t h = std::hash<T>()(value);
  seed ^= h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

} // namespace

std::string CallReturnSummary::calledProcedure() const {
  return callee ? callee->getName().str() : std::string();
}

Transition Transition::makeEdge(std::uint32_t id, const llvm::BasicBlock *src,
                                const llvm::BasicBlock *dst,
                                std::uint32_t sourceOrdinal,
                                std::uint32_t targetOrdinal,
                                const llvm::Instruction *segmentStart,
                                const llvm::Instruction *stopBefore) {
  Transition t;
  t.kind = TransitionKind::Edge;
  t.id = id;
  t.source = const_cast<llvm::BasicBlock *>(src);
  t.target = const_cast<llvm::BasicBlock *>(dst);
  t.sourceOrdinal = sourceOrdinal;
  t.targetOrdinal = targetOrdinal;
  t.callee = nullptr;
  t.segmentStart = segmentStart;
  t.stopBefore = stopBefore;
  t.call = nullptr;
  return t;
}

Transition Transition::makeMarker(std::uint32_t id,
                                  const llvm::BasicBlock *markedTarget) {
  Transition t;
  t.kind = TransitionKind::Marker;
  t.id = id;
  t.source = nullptr;
  t.target = const_cast<llvm::BasicBlock *>(markedTarget);
  t.sourceOrdinal = 0;
  t.targetOrdinal = 0;
  t.callee = nullptr;
  t.segmentStart = nullptr;
  t.stopBefore = nullptr;
  t.call = nullptr;
  return t;
}

Transition Transition::makeReturnSummary(
    std::uint32_t id, const llvm::BasicBlock *src, const llvm::BasicBlock *dst,
    std::uint32_t sourceOrdinal, std::uint32_t targetOrdinal,
    const llvm::Function *calleeFn, const llvm::CallBase *callSite) {
  Transition t;
  t.kind = TransitionKind::ReturnSummary;
  t.id = id;
  t.source = const_cast<llvm::BasicBlock *>(src);
  t.target = const_cast<llvm::BasicBlock *>(dst);
  t.sourceOrdinal = sourceOrdinal;
  t.targetOrdinal = targetOrdinal;
  t.callee = const_cast<llvm::Function *>(calleeFn);
  t.segmentStart = nullptr;
  t.stopBefore = nullptr;
  t.call = callSite;
  return t;
}

Transition Transition::makeEnterCall(std::uint32_t id,
                                     const llvm::BasicBlock *src,
                                     const llvm::BasicBlock *calleeEntry,
                                     std::uint32_t sourceOrdinal,
                                     std::uint32_t targetOrdinal,
                                     const llvm::Function *calleeFn,
                                     const llvm::CallBase *callSite) {
  Transition t;
  t.kind = TransitionKind::EnterCall;
  t.id = id;
  t.source = const_cast<llvm::BasicBlock *>(src);
  t.target = const_cast<llvm::BasicBlock *>(calleeEntry);
  t.sourceOrdinal = sourceOrdinal;
  t.targetOrdinal = targetOrdinal;
  t.callee = const_cast<llvm::Function *>(calleeFn);
  t.segmentStart = nullptr;
  t.stopBefore = nullptr;
  t.call = callSite;
  return t;
}

Transition Transition::from(const LocationMarkerTransition &m) {
  return makeMarker(m.uniqueId, m.markedTarget);
}

Transition Transition::from(const CallReturnSummary &c) {
  return makeReturnSummary(c.id, c.source, c.target, 0, 0, c.callee);
}

llvm::Optional<LocationMarkerTransition>
Transition::getLocationMarkerTransition() const {
  if (kind != TransitionKind::Marker)
    return llvm::None;
  LocationMarkerTransition m;
  m.markedTarget = target;
  m.uniqueId = id;
  return m;
}

llvm::Optional<CallReturnSummary> Transition::getCallReturnSummary() const {
  if (kind != TransitionKind::ReturnSummary)
    return llvm::None;
  CallReturnSummary c;
  c.source = source;
  c.target = target;
  c.callee = callee;
  c.call = call;
  c.id = id;
  return c;
}

bool Transition::operator==(const Transition &o) const {
  return kind == o.kind && id == o.id && source == o.source &&
         target == o.target && sourceOrdinal == o.sourceOrdinal &&
         targetOrdinal == o.targetOrdinal && callee == o.callee &&
         segmentStart == o.segmentStart && stopBefore == o.stopBefore &&
         call == o.call;
}

std::size_t hashValue(const Transition &t) {
  std::size_t seed = 0;
  combineHash(seed, static_cast<std::size_t>(t.kind));
  combineHash(seed, static_cast<std::size_t>(t.id));
  combineHash(seed, t.source);
  combineHash(seed, t.target);
  combineHash(seed, static_cast<std::size_t>(t.sourceOrdinal));
  combineHash(seed, static_cast<std::size_t>(t.targetOrdinal));
  combineHash(seed, t.callee);
  combineHash(seed, t.segmentStart);
  combineHash(seed, t.stopBefore);
  combineHash(seed, t.call);
  return seed;
}

std::ostream &operator<<(std::ostream &os, const Transition &t) {
  switch (t.kind) {
  case TransitionKind::Edge:
    os << "t" << t.id;
    break;
  case TransitionKind::Marker:
    os << "※" << t.id;
    break;
  case TransitionKind::ReturnSummary:
    os << "ret@" << t.id;
    if (t.callee) {
      os << "(" << t.callee->getName().str() << ")";
    }
    break;
  case TransitionKind::EnterCall:
    os << "enter@" << t.id;
    if (t.callee) {
      os << "(" << t.callee->getName().str() << ")";
    }
    break;
  }
  return os;
}

} // namespace sifa
} // namespace lotus
