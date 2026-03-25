#ifndef PCOMB_LAZY_PARSER_H
#define PCOMB_LAZY_PARSER_H

#include "Utils/Formats/pcomb/Parser/Parser.h"

#include <memory>

namespace pcomb {

// LazyParser allows the user to declare a parser first before giving its full
// definitions. This is useful for recursive grammar definitions. It is
// essentially a pointer to another parser.
template <typename O> class LazyParser : public Parser<O> {
private:
  const Parser<O> *parser;

public:
  using OutputType = O;
  using ResultType = typename Parser<O>::ResultType;

  LazyParser() : parser(nullptr) {}

  LazyParser<OutputType> &setParser(const Parser<OutputType> &p) {
    parser = &p;
    return *this;
  }

  ResultType parse(const InputStream &input) const {
    assert(parser != nullptr);
    return parser->parse(input);
  }
};

} // namespace pcomb

#endif
