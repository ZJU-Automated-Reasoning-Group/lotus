#ifndef PCOMB_PARSER_ADAPTER_H
#define PCOMB_PARSER_ADAPTER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

namespace pcomb {

// The ParserAdapter combinator takes another parser p0, fails if p0 fails, and
// convert p0's result to T using the function specified by the user if
// succeeds.
template <typename Converter, typename ParserA>
class ParserAdapter
    : public Parser<std::result_of_t<Converter(typename ParserA::OutputType)>> {
private:
  ParserA pa;
  Converter conv;

  using AdapterInputType = typename ParserA::OutputType;
  using AdapterOutputType = std::result_of_t<Converter(AdapterInputType)>;

public:
  using OutputType = AdapterOutputType;
  using ResultType = typename Parser<OutputType>::ResultType;

  template <typename PA, typename C>
  ParserAdapter(PA &&p, C &&c)
      : pa(std::forward<PA>(p)), conv(std::forward<C>(c)) {}

  ResultType parse(const InputStream &input) const override {
    auto pResult = pa.parse(input);
    auto ret = ResultType(pResult.getInputStream());
    if (pResult.success())
      ret.setOutput(conv(std::move(pResult).getOutput()));
    return ret;
  }
};

template <typename Converter, typename ParserA>
auto rule(ParserA &&p, Converter &&c) {
  using ParserType = std::remove_reference_t<ParserA>;
  return ParserAdapter<Converter, ParserType>(std::forward<ParserA>(p),
                                              std::forward<Converter>(c));
}

} // namespace pcomb

#endif
