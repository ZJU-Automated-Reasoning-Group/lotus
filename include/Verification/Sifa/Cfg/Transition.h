//===-- Verification/Sifa/Cfg/Transition.h --------------------------------===//
//
// Transition labels used for path expressions and Sifa's regex/DAG
// interpretation.
//
// Ultimate-aligned: Ultimate has separate classes LocationMarkerTransition and
// CallReturnSummary (cfgpreprocessing). We provide those as named structs and
// a unified Transition (tagged union) for the path-expression alphabet.
//
// In lotus, Transition::id is typically assigned by the producer:
// - ProcedureGraph assigns dense ids to CFG edges (and ReturnSummary edges).
// - ProcedureResources assigns ids to marker transitions used to denote
// LOIs/EXIT. The id is used for hashing/comparison and as an index into
// side-tables such as ProcedureGraph::transitions().
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_CFG_TRANSITION_H
#define LOTUS_VERIFICATION_SIFA_CFG_TRANSITION_H

#include "llvm/ADT/Optional.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <cstdint>
#include <ostream>
#include <string>

namespace llvm {
class CallBase;
class Instruction;
} // namespace llvm

namespace lotus {
namespace sifa {

enum class TransitionKind : std::uint8_t {
  Edge = 0,
  Marker = 1,
  ReturnSummary = 2,
  EnterCall = 3,
};

//--- Ultimate cfgpreprocessing/LocationMarkerTransition (aligned) -------------
/// Transition to mark paths uniquely in RegexDag (Ultimate
/// LocationMarkerTransition). getSource() returns nullptr for markers;
/// getTarget() returns the marked location.
struct LocationMarkerTransition {
  llvm::BasicBlock *markedTarget = nullptr;
  std::uint32_t uniqueId = 0;

  llvm::BasicBlock *getSource() const { return nullptr; }
  llvm::BasicBlock *getTarget() const { return markedTarget; }
  std::uint32_t getUniqueId() const { return uniqueId; }
};

//--- Ultimate cfgpreprocessing/CallReturnSummary (aligned) -------------------
/// One transition representing: enter callee, execute body, return (Ultimate
/// CallReturnSummary). correspondingCall() / correspondingReturn() in Ultimate
/// return ICFG edges; we expose callee.
struct CallReturnSummary {
  llvm::BasicBlock *source = nullptr;
  llvm::BasicBlock *target = nullptr;
  llvm::Function *callee = nullptr;
  const llvm::CallBase *call = nullptr;
  std::uint32_t id = 0;

  llvm::BasicBlock *getSource() const { return source; }
  llvm::BasicBlock *getTarget() const { return target; }
  llvm::Function *getCallee() const { return callee; }
  /// Ultimate: calledProcedure() / getSucceedingProcedure()
  std::string calledProcedure() const;
};

//--- Unified transition (path-expression alphabet) ---------------------------
/// A labeled transition used as a PathExpressions "letter".
/// - Edge: regular CFG edge (source → target).
/// - Marker: LocationMarkerTransition; target = marked location, source = null.
/// - ReturnSummary: CallReturnSummary; synthetic call+return edge.
/// - EnterCall: synthetic caller -> callee-entry edge used for the "enter the
///   callee without returning" paths in interprocedural Sifa.
///
/// The meaning of a ReturnSummary transition is domain/interpreter-defined.
/// In reachability-style interpretation it can be treated as "jump to
/// successor", while richer interprocedural interpreters may use the embedded
/// callee to consult summaries or recursively interpret the callee.
struct Transition {
  TransitionKind kind = TransitionKind::Edge;
  std::uint32_t id = 0;
  llvm::BasicBlock *source = nullptr;
  llvm::BasicBlock *target = nullptr;
  std::uint32_t sourceOrdinal = 0;
  std::uint32_t targetOrdinal = 0;
  llvm::Function *callee = nullptr;
  const llvm::Instruction *segmentStart = nullptr;
  const llvm::Instruction *stopBefore = nullptr;
  const llvm::CallBase *call = nullptr;

  static Transition makeEdge(std::uint32_t id, const llvm::BasicBlock *src,
                             const llvm::BasicBlock *dst,
                             std::uint32_t sourceOrdinal = 0,
                             std::uint32_t targetOrdinal = 0,
                             const llvm::Instruction *segmentStart = nullptr,
                             const llvm::Instruction *stopBefore = nullptr);
  static Transition makeMarker(std::uint32_t id,
                               const llvm::BasicBlock *markedTarget);
  static Transition
  makeReturnSummary(std::uint32_t id, const llvm::BasicBlock *src,
                    const llvm::BasicBlock *dst, std::uint32_t sourceOrdinal,
                    std::uint32_t targetOrdinal, const llvm::Function *calleeFn,
                    const llvm::CallBase *callSite = nullptr);
  static Transition makeEnterCall(std::uint32_t id, const llvm::BasicBlock *src,
                                  const llvm::BasicBlock *calleeEntry,
                                  std::uint32_t sourceOrdinal,
                                  std::uint32_t targetOrdinal,
                                  const llvm::Function *calleeFn,
                                  const llvm::CallBase *callSite = nullptr);

  /// Ultimate-aligned: build from LocationMarkerTransition.
  static Transition from(const LocationMarkerTransition &m);
  /// Ultimate-aligned: build from CallReturnSummary.
  static Transition from(const CallReturnSummary &c);

  /// When kind == Marker, view as LocationMarkerTransition.
  llvm::Optional<LocationMarkerTransition> getLocationMarkerTransition() const;
  /// When kind == ReturnSummary, view as CallReturnSummary.
  llvm::Optional<CallReturnSummary> getCallReturnSummary() const;

  bool landsAtBlockEntry() const {
    return target != nullptr && targetOrdinal == 0;
  }

  bool operator==(const Transition &o) const;
};

std::size_t hashValue(const Transition &t);

struct TransitionHash {
  std::size_t operator()(const Transition &t) const { return hashValue(t); }
};

std::ostream &operator<<(std::ostream &os, const Transition &t);

} // namespace sifa
} // namespace lotus

namespace std {
template <> struct hash<lotus::sifa::Transition> {
  std::size_t operator()(const lotus::sifa::Transition &t) const {
    return lotus::sifa::hashValue(t);
  }
};
} // namespace std

#endif // LOTUS_VERIFICATION_SIFA_CFG_TRANSITION_H
