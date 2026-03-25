#ifndef PCOMB_SEQ_PARSER_H
#define PCOMB_SEQ_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <tuple>

namespace pcomb {

namespace detail {

// AttrTypeImpl is a (recursive) template to compute the Attribute type of the
// seq parser
template <typename Tuple, size_t I> struct SeqOutputTypeImpl {
private:
  using PrevType = typename SeqOutputTypeImpl<Tuple, I - 1>::type;
  using ParserRefType = std::tuple_element_t<I - 1, Tuple>;
  using ParserType = std::remove_reference_t<ParserRefType>;
  static_assert(std::is_base_of<Parser<typename ParserType::OutputType>,
                                ParserType>::value,
                "SeqParser only accepts parser type");
  using CurrType = typename ParserType::OutputType;

public:
  using type = decltype(std::tuple_cat(std::declval<PrevType>(),
                                       std::declval<std::tuple<CurrType>>()));
};
template <typename Tuple> struct SeqOutputTypeImpl<Tuple, 1> {
private:
  using ParserRefType = std::tuple_element_t<0, Tuple>;
  using ParserType = std::remove_reference_t<ParserRefType>;
  static_assert(std::is_base_of<Parser<typename ParserType::OutputType>,
                                ParserType>::value,
                "SeqParser only accepts parser type");
  using OutputType = typename ParserType::OutputType;

public:
  using type = std::tuple<OutputType>;
};

} // namespace detail

// The SeqParser combinator applies multiple parsers (p0, p1, p2, ...)
// consequtively. If p0 succeeds, it parse the rest of the input string with
// (p1, p2, ...). If one of the parsers fails, the entire combinator fails.
// Otherwise, return the result in a tuple
template <typename... Parsers>
class SeqParser : public Parser<typename detail::SeqOutputTypeImpl<
                      std::tuple<Parsers...>, sizeof...(Parsers)>::type> {
public:
  using OutputType =
      typename detail::SeqOutputTypeImpl<std::tuple<Parsers...>,
                                         sizeof...(Parsers)>::type;
  using ResultType = typename Parser<OutputType>::ResultType;

private:
  std::tuple<Parsers...> parsers;

  template <typename Tuple, size_t I> struct SeqNParserImpl {
    static auto parse(const Tuple &t, const InputStream &input) {
      auto prevRes = SeqNParserImpl<Tuple, I - 1>::parse(t, input);

      using RetType = typename detail::SeqOutputTypeImpl<Tuple, I>::type;
      auto ret = ParseResult<RetType>(prevRes.getInputStream());
      if (prevRes.success()) {
        auto curRes = std::get<I - 1>(t).parse(prevRes.getInputStream());
        if (curRes.success())
          ret = ParseResult<RetType>(
              std::move(curRes).getInputStream(),
              std::tuple_cat(std::move(prevRes).getOutput(),
                             std::make_tuple(std::move(curRes).getOutput())));
      }

      return ret;
    }
  };

  template <typename Tuple> struct SeqNParserImpl<Tuple, 1> {
    static auto parse(const Tuple &t, const InputStream &input) {

      auto res = std::get<0>(t).parse(input);
      using RetType = typename detail::SeqOutputTypeImpl<Tuple, 1>::type;
      auto ret = ParseResult<RetType>(input);
      if (res.success())
        ret = ParseResult<RetType>(std::move(res).getInputStream(),
                                   std::make_tuple(std::move(res).getOutput()));

      return ret;
    }
  };

public:
  SeqParser(Parsers &&...ps) : parsers(std::forward_as_tuple(ps...)) {}

  ResultType parse(const InputStream &input) const {
    return SeqNParserImpl<std::tuple<Parsers...>, sizeof...(Parsers)>::parse(
        parsers, input);
  }
};

template <typename... Parsers> SeqParser<Parsers...> seq(Parsers &&...parsers) {
  return SeqParser<Parsers...>(std::forward<Parsers>(parsers)...);
}

} // namespace pcomb

#endif
