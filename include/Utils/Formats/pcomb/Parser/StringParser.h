#ifndef PCOMB_STRING_PARSER_H
#define PCOMB_STRING_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <llvm/ADT/StringRef.h>

namespace pcomb {

// StringParser matches a string and returns that string as its attribute
class StringParser : public Parser<llvm::StringRef> {
private:
  using StringView = llvm::StringRef;
  StringView pattern;

public:
  using OutputType = StringView;
  using ResultType = typename Parser<StringView>::ResultType;

  StringParser(StringView s) : pattern(s) {}

  ResultType parse(const InputStream &input) const override {
    auto ret = ResultType(input);

    auto inputView = llvm::StringRef(input.getRawBuffer(), pattern.size());
    if (pattern.compare(inputView) == 0)
      ret = ResultType(input.consume(pattern.size()), inputView);

    return ret;
  }
};

inline StringParser str(const llvm::StringRef &s) { return StringParser(s); }

} // namespace pcomb

#endif
