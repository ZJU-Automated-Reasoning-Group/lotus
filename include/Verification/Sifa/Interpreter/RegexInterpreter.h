//===-- Verification/Sifa/Interpreter/RegexInterpreter.h ------------------===//
//
// Interpret path-expression regexes (Utils/Algorithms/PathExpressions/Regex.h)
// using an abstract domain.
//
// Semantics:
//  - ε: identity
//  - ∅: bottom
//  - Literal(label): domain.post(label, in)
//  - Union: join
//  - Concat: sequential composition
//  - Star: lfp X. join(in, inner(X)) with widen
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_REGEXINTERPRETER_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_REGEXINTERPRETER_H

#include "Utils/Algorithms/PathExpressions/Regex.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"

#include <cstddef>
#include <stdexcept>

namespace lotus {
namespace sifa {

struct InterpreterOptions {
  /// Hard cap to avoid non-termination if widen is ineffective.
  std::size_t maxLoopIterations = 32;
};

template <typename LabelT, typename StateT>
class RegexInterpreter final
    : public lotus::pathexpressions::IRegexVisitor<LabelT, StateT,
                                                   const StateT *> {
public:
  using Label = LabelT;
  using State = StateT;
  using RegexRef = lotus::pathexpressions::RegexRef<Label>;
  using Domain = AbstractDomain<Label, State>;

  RegexInterpreter(const Domain &domain, InterpreterOptions options)
      : domain_(domain), options_(options) {}

  State eval(const RegexRef &re, const State &in) {
    if (!re) {
      throw std::invalid_argument("RegexInterpreter::eval received null regex");
    }
    return re->accept(*this, &in);
  }

  State visit(const lotus::pathexpressions::Union<Label> &re,
              const State *in) override {
    const State a = eval(re.getFirst(), *in);
    const State b = eval(re.getSecond(), *in);
    return domain_.join(a, b);
  }

  State visit(const lotus::pathexpressions::Concatenation<Label> &re,
              const State *in) override {
    const State mid = eval(re.getFirst(), *in);
    if (domain_.isBottom(mid)) {
      return domain_.bottom();
    }
    return eval(re.getSecond(), mid);
  }

  State visit(const lotus::pathexpressions::Star<Label> &re,
              const State *in) override {
    // X0 = in
    // Xi+1 = widen(Xi, join(in, inner(Xi)))
    State acc = *in;
    for (std::size_t i = 0; i < options_.maxLoopIterations; ++i) {
      const State bodyOut = eval(re.getInner(), acc);
      const State joined = domain_.join(*in, bodyOut);
      if (domain_.leq(joined, acc)) {
        return acc;
      }
      acc = domain_.widen(acc, joined);
    }
    return acc;
  }

  State visit(const lotus::pathexpressions::Literal<Label> &re,
              const State *in) override {
    if (domain_.isBottom(*in)) {
      return domain_.bottom();
    }
    return domain_.post(re.getLetter(), *in);
  }

  State visit(const lotus::pathexpressions::Epsilon<Label> &re,
              const State *in) override {
    (void)re;
    return *in;
  }

  State visit(const lotus::pathexpressions::EmptySet<Label> &re,
              const State *in) override {
    (void)re;
    (void)in;
    return domain_.bottom();
  }

private:
  const Domain &domain_;
  InterpreterOptions options_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_REGEXINTERPRETER_H
