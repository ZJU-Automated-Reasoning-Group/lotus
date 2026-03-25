#ifndef PCOMB_PARSER_H
#define PCOMB_PARSER_H

#include "Utils/Formats/pcomb/InputStream/InputStream.h"
#include "Utils/Formats/pcomb/Parser/ParseResult.h"

#include <type_traits>

namespace pcomb {

template <typename O> class Parser {
public:
  using OutputType = O;
  using ResultType = ParseResult<OutputType>;

  virtual ~Parser() {}

  virtual ResultType parse(const InputStream &input) const = 0;
};

} // namespace pcomb

#endif
