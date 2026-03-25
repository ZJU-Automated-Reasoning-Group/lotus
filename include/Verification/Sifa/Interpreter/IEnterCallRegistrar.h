//===-- Verification/Sifa/Interpreter/IEnterCallRegistrar.h ---------------===//
//
// Registrar for "enter call" events during DAG interpretation (Ultimate Sifa).
//
// When the interpreter hits a call edge (enter call) it can register the
// callee and the abstract state at entry for later processing (e.g. by
// IcfgInterpreter). No-op implementation can be used when not doing
// interprocedural analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_IENTERCALLREGISTRAR_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_IENTERCALLREGISTRAR_H

#include <string>

namespace lotus {
namespace sifa {

/// Interface for registering enter-call (callee + input state) during
/// interpretation. Ported from Ultimate IEnterCallRegistrar.
template <typename StateT> class IEnterCallRegistrar {
public:
  virtual ~IEnterCallRegistrar() = default;

  virtual void registerEnterCall(const std::string &calleeName,
                                 const StateT &calleeInput) = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_IENTERCALLREGISTRAR_H
