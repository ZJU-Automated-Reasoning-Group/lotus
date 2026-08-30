#include "Solvers/Datalog/Frontend/Frontend.h"

#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>

namespace lotus::datalog::frontend {
namespace {

using internal::Atom;
using internal::BodyItem;
using internal::Expression;
using internal::Fact;
using internal::FrontendIR;
using internal::NegativeItem;
using internal::Output;
using internal::PositiveItem;
using internal::Relation;
using internal::Scalar;
using internal::SourceLocation;
using llvm::StringRef;

struct SExpr {
  bool atom = true;
  std::string value;
  std::vector<SExpr> children;
  std::size_t line = 1;
  std::size_t column = 1;
  std::string source;
};

class SExprParser {
public:
  explicit SExprParser(SourceUnit input)
      : input_(input.content),
        source_(input.name.empty() ? "<z3>" : input.name.str()) {}

  std::vector<SExpr> parseAll() {
    std::vector<SExpr> result;
    skipTrivia();
    while (!atEnd()) {
      result.push_back(parseOne());
      skipTrivia();
    }
    return result;
  }

private:
  [[noreturn]] void fail(std::size_t line, std::size_t column,
                         const std::string &message) const {
    throw FrontendError("syntax_error", source_, line, column, message);
  }

  bool atEnd() const { return offset_ >= input_.size(); }
  char peek() const { return input_[offset_]; }

  char advance() {
    const char result = input_[offset_++];
    if (result == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    return result;
  }

  void skipTrivia() {
    while (!atEnd()) {
      if (std::isspace(static_cast<unsigned char>(peek()))) {
        advance();
        continue;
      }
      if (peek() == ';') {
        while (!atEnd() && advance() != '\n') {
        }
        continue;
      }
      break;
    }
  }

  SExpr parseOne() {
    skipTrivia();
    if (atEnd())
      fail(line_, column_, "unexpected end of input");
    const std::size_t line = line_;
    const std::size_t column = column_;
    if (peek() != '(')
      return parseAtom(line, column);

    advance();
    SExpr list;
    list.atom = false;
    list.line = line;
    list.column = column;
    list.source = source_;
    skipTrivia();
    while (!atEnd() && peek() != ')') {
      list.children.push_back(parseOne());
      skipTrivia();
    }
    if (atEnd())
      fail(line, column, "unterminated list");
    advance();
    return list;
  }

  SExpr parseAtom(std::size_t line, std::size_t column) {
    std::string value;
    if (peek() == '"') {
      value.push_back(advance());
      while (!atEnd()) {
        const char current = advance();
        value.push_back(current);
        if (current == '"')
          break;
        if (current == '\\' && !atEnd())
          value.push_back(advance());
      }
      if (value.back() != '"')
        fail(line, column, "unterminated string");
    } else {
      while (!atEnd() && !std::isspace(static_cast<unsigned char>(peek())) &&
             peek() != '(' && peek() != ')' && peek() != ';')
        value.push_back(advance());
    }
    if (value.empty())
      fail(line, column, "expected atom");
    return {true, std::move(value), {}, line, column, source_};
  }

  StringRef input_;
  std::string source_;
  std::size_t offset_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

class Z3Parser {
public:
  explicit Z3Parser(llvm::ArrayRef<SourceUnit> inputs) {
    for (const SourceUnit &input : inputs) {
      std::vector<SExpr> forms = SExprParser(input).parseAll();
      forms_.insert(forms_.end(), std::make_move_iterator(forms.begin()),
                    std::make_move_iterator(forms.end()));
    }
  }

  FrontendIR parse() {
    // Resolve the declaration environment first. This permits schema, facts,
    // and rules to be supplied as separate source units in any order.
    for (const SExpr &form : forms_) {
      const StringRef name = command(form);
      if (name == "set-option" || name == "set-logic")
        continue;
      if (name == "define-sort")
        parseSort(form);
      else if (name == "declare-rel")
        parseRelation(form);
      else if (name == "declare-var")
        parseVariable(form);
    }
    for (const SExpr &form : forms_) {
      const StringRef name = command(form);
      if (name == "rule")
        parseRule(form);
      else if (name == "query")
        parseQuery(form);
      else if (name != "set-option" && name != "set-logic" &&
               name != "define-sort" && name != "declare-rel" &&
               name != "declare-var")
        fail(form, "unsupported fixedpoint command '" + name.str() + "'");
    }
    return std::move(program_);
  }

private:
  struct Sort {
    std::string json_type;
    unsigned bit_width = 0;
    std::uint64_t finite_size = 0;

    bool operator==(const Sort &other) const {
      return json_type == other.json_type && bit_width == other.bit_width &&
             finite_size == other.finite_size;
    }
  };

  [[noreturn]] static void fail(const SExpr &form, const std::string &message) {
    throw FrontendError("syntax_error", form.source, form.line, form.column,
                        message);
  }

  static SourceLocation location(const SExpr &form) {
    return {form.source, form.line, form.column};
  }

  static const std::string &atom(const SExpr &form) {
    if (!form.atom)
      fail(form, "expected atom");
    return form.value;
  }

  static StringRef command(const SExpr &form) {
    if (form.atom || form.children.empty())
      fail(form, "expected command list");
    return atom(form.children.front());
  }

  void parseSort(const SExpr &form) {
    if (form.children.size() != 4)
      fail(form, "define-sort must have three arguments");
    const std::string name = atom(form.children[1]);
    const SExpr &parameters = form.children[2];
    if (parameters.atom || !parameters.children.empty())
      fail(parameters, "parameterized sort aliases are not supported");
    sorts_[name] = parseSortExpr(form.children[3]);
  }

  Sort parseSortExpr(const SExpr &form) const {
    if (form.atom) {
      auto found = sorts_.find(form.value);
      if (found != sorts_.end())
        return found->second;
      if (form.value == "Bool")
        return {"bool", 0, 0};
      fail(form, "unsupported sort '" + form.value + "'");
    }
    if (form.children.size() != 3 || atom(form.children[0]) != "_")
      fail(form, "expected (_ BitVec N) or (_ FiniteDomain N)");
    const std::string constructor = atom(form.children[1]);
    const std::uint64_t size = parseUnsigned(atom(form.children[2]), form);
    if (constructor == "BitVec") {
      if (size == 0 || size > 64)
        fail(form, "BitVec width must be between 1 and 64");
      return {"u64", static_cast<unsigned>(size), 0};
    }
    if (constructor == "FiniteDomain") {
      if (size == 0)
        fail(form, "FiniteDomain size is outside the portable range");
      return {"u64", 0, size};
    }
    fail(form, "unsupported sort constructor '" + constructor + "'");
  }

  void parseRelation(const SExpr &form) {
    if (form.children.size() != 3)
      fail(form, "declare-rel must have a name and sort list");
    const std::string name = atom(form.children[1]);
    const SExpr &sort_list = form.children[2];
    if (sort_list.atom)
      fail(sort_list, "relation sorts must be a list");
    std::vector<std::string> columns;
    std::vector<Sort> sorts;
    for (const SExpr &sort_expr : sort_list.children) {
      Sort sort = parseSortExpr(sort_expr);
      columns.push_back(sort.json_type);
      sorts.push_back(std::move(sort));
    }
    relation_sorts_[name] = std::move(sorts);
    program_.relations.push_back(
        Relation{name, std::move(columns), false, location(form)});
  }

  void parseVariable(const SExpr &form) {
    if (form.children.size() != 3)
      fail(form, "declare-var must have a name and sort");
    variables_[atom(form.children[1])] = parseSortExpr(form.children[2]);
  }

  void parseRule(const SExpr &form) {
    if (form.children.size() != 2)
      fail(form, "rule must have one Horn clause");
    const SExpr &clause = form.children[1];
    if (!clause.atom && !clause.children.empty() &&
        atom(clause.children[0]) == "=>") {
      if (clause.children.size() != 3)
        fail(clause, "=> must have a body and head");
      std::vector<BodyItem> body;
      parseBody(clause.children[1], body);
      std::vector<Atom> heads;
      heads.push_back(parseRelationAtom(clause.children[2]));
      program_.rules.push_back(
          {std::move(heads), std::move(body), location(form)});
      return;
    }
    program_.facts.push_back(
        Fact{parseRelationAtom(clause), location(form)});
  }

  void parseBody(const SExpr &form, std::vector<BodyItem> &body) const {
    if (form.atom && form.value == "true")
      return;
    if (!form.atom && !form.children.empty() &&
        atom(form.children[0]) == "and") {
      if (form.children.size() == 1)
        fail(form, "empty conjunction is not supported");
      for (std::size_t index = 1; index < form.children.size(); ++index)
        parseBody(form.children[index], body);
      return;
    }
    if (!form.atom && !form.children.empty() &&
        atom(form.children[0]) == "not") {
      if (form.children.size() != 2)
        fail(form, "not must contain one relation atom");
      body.push_back(NegativeItem{parseRelationAtom(form.children[1])});
      return;
    }
    body.push_back(PositiveItem{parseRelationAtom(form)});
  }

  void parseQuery(const SExpr &form) {
    if (form.children.size() < 2)
      fail(form, "query requires a relation");
    const SExpr &target = form.children[1];
    if (!target.atom)
      fail(target, "only whole-relation queries are supported");
    program_.outputs.push_back(Output{target.value, location(target)});
  }

  Atom parseRelationAtom(const SExpr &form) const {
    Atom result;
    result.location = location(form);
    if (form.atom) {
      result.relation = form.value;
    } else {
      if (form.children.empty())
        fail(form, "empty relation application");
      result.relation = atom(form.children[0]);
    }
    auto found = relation_sorts_.find(result.relation);
    if (found == relation_sorts_.end())
      fail(form, "unknown relation '" + result.relation + "'");
    const std::size_t argument_count = form.atom ? 0 : form.children.size() - 1;
    if (argument_count != found->second.size())
      fail(form, "arity mismatch for relation '" + result.relation + "'");
    if (!form.atom) {
      for (std::size_t index = 0; index < argument_count; ++index) {
        result.arguments.push_back(
            parseTerm(form.children[index + 1], found->second[index]));
      }
    }
    return result;
  }

  Expression parseTerm(const SExpr &form, const Sort &expected) const {
    if (!form.atom) {
      if (form.children.size() == 3 && atom(form.children[0]) == "_" &&
          StringRef(atom(form.children[1])).startswith("bv")) {
        const StringRef numeral =
            StringRef(atom(form.children[1])).drop_front(2);
        const std::uint64_t value = parseUnsigned(numeral, form);
        const std::uint64_t width = parseUnsigned(atom(form.children[2]), form);
        if (width == 0 || width > 64 ||
            (width < 64 && value >= (std::uint64_t{1} << width)))
          fail(form, "invalid bit-vector numeral");
        if (expected.json_type != "u64" || expected.bit_width != width)
          fail(form, "bit-vector constant sort does not match relation column");
        return constant(value, form);
      }
      fail(form, "only relation atoms and bit-vector constants are supported");
    }

    auto variable = variables_.find(form.value);
    if (variable != variables_.end()) {
      if (!(variable->second == expected))
        fail(form, "variable sort does not match relation column");
      Expression result;
      result.kind = Expression::Kind::Variable;
      result.name = form.value;
      result.location = location(form);
      return result;
    }
    if (form.value == "true" || form.value == "false") {
      if (expected.json_type != "bool")
        fail(form, "Boolean constant sort does not match relation column");
      return scalar(form.value == "true");
    }
    if (StringRef(form.value).startswith("#b")) {
      const StringRef digits = StringRef(form.value).drop_front(2);
      if (expected.json_type != "u64" || expected.bit_width != digits.size())
        fail(form, "bit-vector constant sort does not match relation column");
      return constant(parseRadix(StringRef(form.value).drop_front(2), 2, form),
                      form);
    }
    if (StringRef(form.value).startswith("#x")) {
      const StringRef digits = StringRef(form.value).drop_front(2);
      if (expected.json_type != "u64" ||
          expected.bit_width != digits.size() * 4)
        fail(form, "bit-vector constant sort does not match relation column");
      return constant(parseRadix(StringRef(form.value).drop_front(2), 16, form),
                      form);
    }
    if (expected.json_type == "u64" && expected.bit_width == 0) {
      const std::uint64_t value = parseUnsigned(form.value, form);
      if (expected.finite_size != 0 && value >= expected.finite_size)
        fail(form, "finite-domain constant is outside the declared domain");
      return constant(value, form);
    }
    fail(form, "unsupported term '" + form.value + "'");
  }

  static Expression constant(std::uint64_t value, const SExpr &form) {
    return scalar(value, location(form));
  }

  template <typename T>
  static Expression scalar(T value, SourceLocation location = {}) {
    Expression result;
    result.kind = Expression::Kind::Constant;
    result.constant = Scalar{std::move(value)};
    result.location = std::move(location);
    return result;
  }

  static std::uint64_t parseUnsigned(StringRef text, const SExpr &form) {
    if (text.empty())
      fail(form, "expected unsigned integer");
    std::uint64_t value = 0;
    for (char digit : text) {
      if (!std::isdigit(static_cast<unsigned char>(digit)))
        fail(form, "expected unsigned integer");
      const unsigned numeric = static_cast<unsigned>(digit - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - numeric) / 10)
        fail(form, "unsigned integer is too large");
      value = value * 10 + numeric;
    }
    return value;
  }

  static std::uint64_t parseRadix(StringRef text, unsigned radix,
                                  const SExpr &form) {
    if (text.empty())
      fail(form, "empty bit-vector literal");
    std::uint64_t value = 0;
    for (char digit : text) {
      unsigned numeric = 0;
      if (digit >= '0' && digit <= '9')
        numeric = static_cast<unsigned>(digit - '0');
      else if (digit >= 'a' && digit <= 'f')
        numeric = static_cast<unsigned>(digit - 'a' + 10);
      else if (digit >= 'A' && digit <= 'F')
        numeric = static_cast<unsigned>(digit - 'A' + 10);
      else
        fail(form, "invalid bit-vector literal");
      if (numeric >= radix ||
          value > (std::numeric_limits<std::uint64_t>::max() - numeric) / radix)
        fail(form, "bit-vector literal is too large");
      value = value * radix + numeric;
    }
    return value;
  }

  std::vector<SExpr> forms_;
  FrontendIR program_;
  std::unordered_map<std::string, Sort> sorts_;
  std::unordered_map<std::string, Sort> variables_;
  std::unordered_map<std::string, std::vector<Sort>> relation_sorts_;
};

} // namespace

std::string translateZ3ToJson(StringRef input) {
  return translateZ3ToJson(SourceUnit{"<z3>", input});
}

std::string translateZ3ToJson(llvm::ArrayRef<SourceUnit> inputs) {
  if (inputs.empty())
    throw std::invalid_argument("no Z3 input sources were provided");
  return internal::toJson(internal::parseZ3(inputs));
}

internal::FrontendIR internal::parseZ3(llvm::ArrayRef<SourceUnit> inputs) {
  if (inputs.empty())
    throw std::invalid_argument("no Z3 input sources were provided");
  return Z3Parser(inputs).parse();
}

} // namespace lotus::datalog::frontend
