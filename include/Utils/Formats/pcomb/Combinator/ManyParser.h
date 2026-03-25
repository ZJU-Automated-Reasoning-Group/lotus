#ifndef PCOMB_MANY_PARSER_H
#define PCOMB_MANY_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <vector>

namespace pcomb {

// The ManyParser combinator applies one parser p0 repeatedly. The result of
// each application of p0 is pushed into a vector, which is the result of the
// entire combinator. If nonEmpty is true, the combinator will fail if the
// vector is empty.
template <typename ParserA>
class ManyParser : Parser<std::vector<typename ParserA::OutputType>> {
private:
  static_assert(
      std::is_base_of<Parser<typename ParserA::OutputType>, ParserA>::value,
      "ManyParser only accepts parser type");

  ParserA pa;
  unsigned minOccurrence;

public:
  using OutputType = std::vector<typename ParserA::OutputType>;
  using ResultType = typename Parser<OutputType>::ResultType;

  ManyParser(const ParserA &a, unsigned n = 0) : pa(a), minOccurrence(n) {}
  ManyParser(ParserA &&a, unsigned n = 0)
      : pa(std::move(a)), minOccurrence(n) {}

  ResultType parse(const InputStream &input) const override {
    auto retVec = OutputType();
    auto resStream = input;

    while (true) {
      auto paResult = pa.parse(resStream);
      if (!paResult.success())
        break;

      retVec.emplace_back(std::move(paResult).getOutput());
      resStream = std::move(paResult).getInputStream();
    }

    ResultType ret(resStream);
    if (retVec.size() >= minOccurrence)
      ret.setOutput(std::move(retVec));
    return ret;
  }
};

template <typename ParserA>
auto many(ParserA &&p0, unsigned minOccurrence = 0) {
  using ParserType = std::remove_reference_t<ParserA>;
  return ManyParser<ParserType>(std::forward<ParserA>(p0), minOccurrence);
}

} // namespace pcomb

#endif
