#ifndef PCOMB_TOKEN_PARSER_H
#define PCOMB_TOKEN_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <bitset>

namespace pcomb {

// TokenParser takes a parser p and strip the whitespace of the input string
// before feed it into p
template <typename ParserA>
class TokenParser : public Parser<typename ParserA::OutputType> {
private:
  auto static constexpr CharSize = sizeof(char) * 8;

  ParserA pa;
  std::bitset<1 << CharSize> charBits;

  void setCharBits(const llvm::StringRef &whitespaces) {
    for (auto ch : whitespaces)
      charBits.set(static_cast<unsigned char>(ch));
  }

public:
  static_assert(
      std::is_base_of<Parser<typename ParserA::OutputType>, ParserA>::value,
      "TokenParser only accepts parser type");

  using OutputType = typename Parser<typename ParserA::OutputType>::OutputType;
  using ResultType = typename Parser<typename ParserA::OutputType>::ResultType;

  TokenParser(const ParserA &p, const llvm::StringRef &w) : pa(p) {
    setCharBits(w);
  }
  TokenParser(ParserA &&p, const llvm::StringRef &w) : pa(std::move(p)) {
    setCharBits(w);
  }

  ResultType parse(const InputStream &input) const override {
    auto resStream = input;

    while (!resStream.isEOF()) {
      auto firstChar = resStream.getRawBuffer()[0];
      if (charBits.test(static_cast<unsigned char>(firstChar)))
        resStream = resStream.consume(1);
      else
        break;
    }
    return pa.parse(resStream);
  }
};

template <typename ParserA>
auto token(ParserA &&p, const llvm::StringRef &w = " \t\n\v\f\r") {
  using ParserType = std::remove_reference_t<ParserA>;
  return TokenParser<ParserType>(std::forward<ParserA>(p), w);
}

} // namespace pcomb

#endif
