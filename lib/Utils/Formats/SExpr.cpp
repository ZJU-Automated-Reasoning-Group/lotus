#include "Utils/Formats/SExpr.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

typedef std::logic_error parse_error;

static bool endoftok(int c) {
  return c == EOF || std::isspace(c) || c == ')' || c == '(';
}

std::istream &operator>>(std::istream &is, SExpr &sexp) {
  std::ws(is);
  int c = is.get();
  assert(!(std::isspace(c)));
  if (c == EOF)
    throw parse_error("Got EOF");
  if (c == ')')
    throw parse_error("Unmatched closing paren");
  if (c == '(') {
    std::vector<SExpr> elems;
    while (std::ws(is).peek() != ')') {
      elems.emplace_back();
      is >> elems.back();
    }
    c = is.get();
    assert(c == ')');
    sexp = SExpr(std::move(elems));
  } else {
    std::string str;
    for (; !endoftok(c); c = is.get())
      str.push_back(c);
    is.putback(c);
    char *end;
    int num = std::strtol(str.c_str(), &end, 10);
    if ((end - str.c_str()) != int_fast64_t(str.size())) {
      sexp = SExpr(str);
    } else {
      sexp = SExpr(num);
    }
  }
  return is;
}

std::ostream &operator<<(std::ostream &os, const SExpr &sexp) {
  switch (sexp.kind()) {
  case SExpr::TOKEN:
    return os << sexp.token().name;
  case SExpr::INT:
    return os << sexp.num().value;
  case SExpr::LIST: {
    os << "(";
    const auto &l = sexp.list().elems;
    auto i = l.begin();
    if (i != l.end()) {
      while (true) {
        os << *i;
        ++i;
        if (i == l.end())
          break;
        os << " ";
      }
    }
    return os << ")";
  }
  default:
    abort();
  }
}