//===-- Verification/Sifa/Interpreter/ErrorOnEnterCall.h ------------------===//
//
// IEnterCallRegistrar that throws on registerEnterCall (Ultimate-aligned).
//
// Use when not expecting any enter-call transitions (e.g. intraprocedural).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_ERRORONENTERCALL_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_ERRORONENTERCALL_H

#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"

#include <stdexcept>
#include <string>

namespace lotus {
namespace sifa {

template <typename StateT>
class ErrorOnEnterCall final : public IEnterCallRegistrar<StateT> {
public:
  /// Ultimate-aligned: ErrorOnEnterCall.instance() — singleton for
  /// intraprocedural use.
  static ErrorOnEnterCall &instance() {
    static ErrorOnEnterCall inst;
    return inst;
  }

  void registerEnterCall(const std::string &calleeName,
                         const StateT &calleeInput) override {
    (void)calleeInput;
    throw std::logic_error(
        "Did not expect any enter calls but received enter call " + calleeName +
        ".");
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_ERRORONENTERCALL_H
