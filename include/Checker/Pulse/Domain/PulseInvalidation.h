#ifndef CHECKER_PULSE_PULSEINVALIDATION_H
#define CHECKER_PULSE_PULSEINVALIDATION_H

#include <string>

namespace llvm {
class Instruction;
} // namespace llvm

namespace pulse {

/**
 * Invalidation kind: how memory was invalidated (Infer-aligned).
 * Used for richer diagnostics and decompiler-style traces.
 */
enum class InvalidationKind {
  CFree,
  CppDelete,
  CppDeleteArray,
  GoneOutOfScope,
  Realloc,
  FClose,
  LockReleased,
  Other
};

/** Human-readable description for reporting. */
inline const char *invalidationKindString(InvalidationKind k) {
  switch (k) {
  case InvalidationKind::CFree:
    return "freed (free)";
  case InvalidationKind::CppDelete:
    return "freed (delete)";
  case InvalidationKind::CppDeleteArray:
    return "freed (delete[])";
  case InvalidationKind::GoneOutOfScope:
    return "gone out of scope";
  case InvalidationKind::Realloc:
    return "invalidated (realloc)";
  case InvalidationKind::FClose:
    return "closed (fclose)";
  case InvalidationKind::LockReleased:
    return "released (unlock)";
  case InvalidationKind::Other:
  default:
    return "invalidated";
  }
}

} // namespace pulse

#endif // CHECKER_PULSE_PULSEINVALIDATION_H
