#ifndef PCOMB_ALT_PARSER_H
#define PCOMB_ALT_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <tuple>

namespace pcomb {

namespace detail {

template <typename Tuple, size_t I> struct AltOutputTypeImpl {
private:
  using PrevType = typename AltOutputTypeImpl<Tuple, I - 1>::type;
  using ParserRefType = std::tuple_element_t<I - 1, Tuple>;
  using ParserType = std::remove_reference_t<ParserRefType>;
  static_assert(std::is_base_of<Parser<typename ParserType::OutputType>,
                                ParserType>::value,
                "AltParser only accepts parser type");
  using CurrType = typename ParserType::OutputType;

public:
  using type = std::common_type_t<PrevType, CurrType>;
};
template <typename Tuple> struct AltOutputTypeImpl<Tuple, 1> {
private:
  using ParserRefType = std::tuple_element_t<0, Tuple>;
  using ParserType = std::remove_reference_t<ParserRefType>;
  static_assert(std::is_base_of<Parser<typename ParserType::OutputType>,
                                ParserType>::value,
                "AltParser only accepts parser type");

public:
  using type = typename ParserType::OutputType;
};

} // namespace detail

// The AltParser combinator applies multiple parser (p0, p1, p2, ...) in turn.
// If p0 succeeds, it returns what p0 returns; otherwise, it tries p1 and return
// what p1 returns if it succeeds; otherwise, try p2, and so on
template <typename... Parsers>
class AltParser : public Parser<typename detail::AltOutputTypeImpl<
                      std::tuple<Parsers...>, sizeof...(Parsers)>::type> {
public:
  using OutputType =
      typename detail::AltOutputTypeImpl<std::tuple<Parsers...>,
                                         sizeof...(Parsers)>::type;
  using ResultType = typename Parser<OutputType>::ResultType;

private:
  std::tuple<Parsers...> parsers;

  template <typename Tuple, size_t I> struct AltNParserImpl {
    static auto parse(const Tuple &t, const InputStream &input) {
      auto constexpr tupleId = std::tuple_size<Tuple>::value - I;
      auto res = std::get<tupleId>(t).parse(input);
      if (res.success())
        return res;
      else
        return AltNParserImpl<Tuple, I - 1>::parse(t, input);
    }
  };

  template <typename Tuple> struct AltNParserImpl<Tuple, 1> {
    static auto parse(const Tuple &t, const InputStream &input) {
      auto constexpr tupleId = std::tuple_size<Tuple>::value - 1;
      return std::get<tupleId>(t).parse(input);
    }
  };

public:
  AltParser(Parsers &&...ps) : parsers(std::forward_as_tuple(ps...)) {}

  ResultType parse(const InputStream &input) const {
    return AltNParserImpl<std::tuple<Parsers...>, sizeof...(Parsers)>::parse(
        parsers, input);
  }
};

template <typename... Parsers> AltParser<Parsers...> alt(Parsers &&...parsers) {
  return AltParser<Parsers...>(std::forward<Parsers>(parsers)...);
}

} // namespace pcomb

#endif
