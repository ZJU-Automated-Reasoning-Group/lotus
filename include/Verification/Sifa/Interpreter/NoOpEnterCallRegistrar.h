//===-- Verification/Sifa/Interpreter/NoOpEnterCallRegistrar.h
//-------------===//
//
// No-op implementation of IEnterCallRegistrar (e.g. intraprocedural only).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_NOOPENTERCALLREGISTRAR_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_NOOPENTERCALLREGISTRAR_H

#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"

#include <string>

namespace lotus {
namespace sifa {

template <typename StateT>
class NoOpEnterCallRegistrar final : public IEnterCallRegistrar<StateT> {
public:
  void registerEnterCall(const std::string &calleeName,
                         const StateT &calleeInput) override {
    (void)calleeName;
    (void)calleeInput;
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_NOOPENTERCALLREGISTRAR_H
