#ifndef PCOMB_REGEX_PARSER_H
#define PCOMB_REGEX_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <regex>

#include <llvm/ADT/StringRef.h>

namespace pcomb {

// RegexParser takes a StringRef as regex and mathes the start of the input
// string against that regex. RegexParser is strictly more powerful than
// StringParser. But I would expect that StringParser is cheaper.
class RegexParser : public Parser<llvm::StringRef> {
private:
  using StringView = llvm::StringRef;
  std::regex regex;

public:
  using OutputType = StringView;
  using ResultType = typename Parser<StringView>::ResultType;

  RegexParser(StringView r) : regex(r.data()) {}

  ResultType parse(const InputStream &input) const override {
    auto ret = ResultType(input);

    auto res = std::cmatch();
    const auto *rawStr = input.getRawBuffer();
    if (std::regex_search(rawStr, res, regex,
                          std::regex_constants::match_continuous)) {
      auto matchLen = res.length(0);
      ret = ResultType(input.consume(matchLen), StringView(rawStr, matchLen));
    }

    return ret;
  }
};

inline RegexParser regex(const llvm::StringRef &s) { return RegexParser(s); }

} // namespace pcomb

#endif
