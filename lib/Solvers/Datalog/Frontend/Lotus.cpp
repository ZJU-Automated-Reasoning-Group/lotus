#include "Solvers/Datalog/Frontend/Frontend.h"
#include "FrontendInternal.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/StringRef.h>

namespace lotus::datalog::frontend {
namespace {

using internal::AggregateItem;
using internal::Atom;
using internal::BodyItem;
using internal::Expression;
using internal::Fact;
using internal::FilterItem;
using internal::FrontendIR;
using internal::Include;
using internal::NegativeItem;
using internal::Output;
using internal::PositiveItem;
using internal::Relation;
using internal::Scalar;
using internal::SourceLocation;
using llvm::StringRef;

enum class DatalogTokenKind {
  End,
  Identifier,
  Integer,
  Float,
  String,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
  Colon,
  Dot,
  RuleArrow,
  Equal,
  EqualEqual,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Bang,
  AndAnd,
  OrOr,
};

struct DatalogToken {
  DatalogTokenKind kind = DatalogTokenKind::End;
  std::string text;
  std::size_t line = 1;
  std::size_t column = 1;
};

class DatalogLexer {
public:
  DatalogLexer(StringRef input, StringRef source)
      : input_(input), source_(source.str()) {}

  DatalogToken next() {
    skipTrivia();
    const std::size_t line = line_;
    const std::size_t column = column_;
    if (atEnd())
      return {DatalogTokenKind::End, "", line, column};

    const char current = peek();
    if (isIdentifierStart(current)) {
      std::string text;
      do {
        text.push_back(advance());
      } while (!atEnd() && isIdentifierContinue(peek()));
      return {DatalogTokenKind::Identifier, std::move(text), line, column};
    }
    if (current == '"')
      return lexString(line, column);
    if (std::isdigit(static_cast<unsigned char>(current)))
      return lexNumber(line, column);

    advance();
    switch (current) {
    case '(':
      return {DatalogTokenKind::LParen, "(", line, column};
    case ')':
      return {DatalogTokenKind::RParen, ")", line, column};
    case '[':
      return {DatalogTokenKind::LBracket, "[", line, column};
    case ']':
      return {DatalogTokenKind::RBracket, "]", line, column};
    case ',':
      return {DatalogTokenKind::Comma, ",", line, column};
    case ';':
      return {DatalogTokenKind::Semicolon, ";", line, column};
    case '.':
      return {DatalogTokenKind::Dot, ".", line, column};
    case ':':
      if (!atEnd() && peek() == '-') {
        advance();
        return {DatalogTokenKind::RuleArrow, ":-", line, column};
      }
      return {DatalogTokenKind::Colon, ":", line, column};
    case '=':
      if (!atEnd() && peek() == '=') {
        advance();
        return {DatalogTokenKind::EqualEqual, "==", line, column};
      }
      return {DatalogTokenKind::Equal, "=", line, column};
    case '!':
      if (!atEnd() && peek() == '=') {
        advance();
        return {DatalogTokenKind::NotEqual, "!=", line, column};
      }
      return {DatalogTokenKind::Bang, "!", line, column};
    case '<':
      if (!atEnd() && peek() == '=') {
        advance();
        return {DatalogTokenKind::LessEqual, "<=", line, column};
      }
      return {DatalogTokenKind::Less, "<", line, column};
    case '>':
      if (!atEnd() && peek() == '=') {
        advance();
        return {DatalogTokenKind::GreaterEqual, ">=", line, column};
      }
      return {DatalogTokenKind::Greater, ">", line, column};
    case '+':
      return {DatalogTokenKind::Plus, "+", line, column};
    case '-':
      return {DatalogTokenKind::Minus, "-", line, column};
    case '*':
      return {DatalogTokenKind::Star, "*", line, column};
    case '/':
      return {DatalogTokenKind::Slash, "/", line, column};
    case '%':
      return {DatalogTokenKind::Percent, "%", line, column};
    case '&':
      if (!atEnd() && peek() == '&') {
        advance();
        return {DatalogTokenKind::AndAnd, "&&", line, column};
      }
      fail(line, column, "expected '&&'");
    case '|':
      if (!atEnd() && peek() == '|') {
        advance();
        return {DatalogTokenKind::OrOr, "||", line, column};
      }
      fail(line, column, "expected '||'");
    default:
      fail(line, column, std::string("unexpected character '") + current + "'");
    }
  }

private:
  static bool isIdentifierStart(char value) {
    return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
  }

  static bool isIdentifierContinue(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
  }

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
      const bool slash_comment = peek() == '/' && offset_ + 1 < input_.size() &&
                                 input_[offset_ + 1] == '/';
      if (peek() == '#' || slash_comment) {
        while (!atEnd() && advance() != '\n') {
        }
        continue;
      }
      break;
    }
  }

  DatalogToken lexString(std::size_t line, std::size_t column) {
    advance();
    std::string result;
    while (!atEnd()) {
      const char current = advance();
      if (current == '"')
        return {DatalogTokenKind::String, std::move(result), line, column};
      if (current != '\\') {
        result.push_back(current);
        continue;
      }
      if (atEnd())
        break;
      switch (const char escaped = advance()) {
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      case '\\':
      case '"':
        result.push_back(escaped);
        break;
      default:
        fail(line, column, "unsupported string escape");
      }
    }
    fail(line, column, "unterminated string literal");
  }

  DatalogToken lexNumber(std::size_t line, std::size_t column) {
    std::string text;
    while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
      text.push_back(advance());
    bool floating = false;
    if (!atEnd() && peek() == '.' && offset_ + 1 < input_.size() &&
        std::isdigit(static_cast<unsigned char>(input_[offset_ + 1]))) {
      floating = true;
      text.push_back(advance());
      while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        text.push_back(advance());
    }
    if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
      floating = true;
      text.push_back(advance());
      if (!atEnd() && (peek() == '+' || peek() == '-'))
        text.push_back(advance());
      if (atEnd() || !std::isdigit(static_cast<unsigned char>(peek())))
        fail(line, column, "invalid floating-point exponent");
      while (!atEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        text.push_back(advance());
    }
    return {floating ? DatalogTokenKind::Float : DatalogTokenKind::Integer,
            std::move(text), line, column};
  }

  StringRef input_;
  std::string source_;
  std::size_t offset_ = 0;
  std::size_t line_ = 1;
  std::size_t column_ = 1;
};

class DatalogParser {
public:
  explicit DatalogParser(SourceUnit input)
      : source_(input.name.empty() ? "<datalog>" : input.name.str()),
        lexer_(input.content, source_), token_(lexer_.next()) {}

  FrontendIR parse() {
    while (token_.kind != DatalogTokenKind::End) {
      if (accept(DatalogTokenKind::Dot)) {
        const DatalogToken directive = expect(DatalogTokenKind::Identifier);
        if (directive.text == "decl")
          parseDeclaration();
        else if (directive.text == "output")
          parseOutput();
        else if (directive.text == "include")
          parseInclude(directive);
        else
          fail(directive, "unsupported directive '." + directive.text + "'");
      } else {
        parseClause();
      }
    }
    return std::move(program_);
  }

private:
  [[noreturn]] void fail(const DatalogToken &token,
                         const std::string &message) const {
    throw FrontendError("syntax_error", source_, token.line, token.column,
                        message);
  }

  SourceLocation location(const DatalogToken &token) const {
    return {source_, token.line, token.column};
  }

  bool accept(DatalogTokenKind kind) {
    if (token_.kind != kind)
      return false;
    token_ = lexer_.next();
    return true;
  }

  DatalogToken expect(DatalogTokenKind kind) {
    if (token_.kind != kind)
      fail(token_, "unexpected token '" + token_.text + "'");
    DatalogToken result = token_;
    token_ = lexer_.next();
    return result;
  }

  void parseDeclaration() {
    const DatalogToken name_token = expect(DatalogTokenKind::Identifier);
    const std::string name = name_token.text;
    expect(DatalogTokenKind::LParen);
    std::vector<std::string> columns;
    if (!accept(DatalogTokenKind::RParen)) {
      do {
        (void)expect(DatalogTokenKind::Identifier);
        expect(DatalogTokenKind::Colon);
        columns.push_back(parseType());
      } while (accept(DatalogTokenKind::Comma));
      expect(DatalogTokenKind::RParen);
    }
    bool lattice = !columns.empty() && isLatticeType(columns.back());
    if (token_.kind == DatalogTokenKind::Identifier &&
        token_.text == "lattice") {
      token_ = lexer_.next();
      lattice = true;
    }
    program_.relations.push_back(
        Relation{name, std::move(columns), lattice, location(name_token)});
  }

  void parseOutput() {
    const DatalogToken relation = expect(DatalogTokenKind::Identifier);
    program_.outputs.push_back(Output{relation.text, location(relation)});
  }

  void parseInclude(const DatalogToken &directive) {
    const DatalogToken path = expect(DatalogTokenKind::String);
    if (path.text.empty())
      fail(path, "include path must not be empty");
    program_.includes.push_back(Include{path.text, location(directive)});
  }

  void parseClause() {
    const DatalogToken start = token_;
    std::vector<Atom> heads;
    heads.push_back(parseAtom(true));
    while (accept(DatalogTokenKind::Semicolon))
      heads.push_back(parseAtom(true));
    if (accept(DatalogTokenKind::RuleArrow)) {
      std::vector<BodyItem> body;
      do {
        body.push_back(parseBodyItem());
      } while (accept(DatalogTokenKind::Comma));
      expect(DatalogTokenKind::Dot);
      program_.rules.push_back(
          {std::move(heads), std::move(body), location(start)});
      return;
    }
    if (heads.size() != 1)
      fail(token_, "multiple heads require a rule body");
    expect(DatalogTokenKind::Dot);
    program_.facts.push_back(Fact{std::move(heads.front()), location(start)});
  }

  std::string parseType() {
    const DatalogToken base = expect(DatalogTokenKind::Identifier);
    if (base.text != "min" && base.text != "max" && base.text != "set")
      return base.text;
    expect(DatalogTokenKind::Less);
    const std::string element = expect(DatalogTokenKind::Identifier).text;
    expect(DatalogTokenKind::Greater);
    return base.text + "<" + element + ">";
  }

  static bool isLatticeType(StringRef type) {
    return type.startswith("min<") || type.startswith("max<") ||
           type.startswith("set<");
  }

  BodyItem parseBodyItem() {
    if (accept(DatalogTokenKind::Bang))
      return NegativeItem{parseAtom(false)};
    if (token_.kind == DatalogTokenKind::Identifier && token_.text == "not") {
      token_ = lexer_.next();
      return NegativeItem{parseAtom(false)};
    }
    if (token_.kind == DatalogTokenKind::Identifier && token_.text == "where") {
      token_ = lexer_.next();
      return FilterItem{parseExpression()};
    }
    if (token_.kind == DatalogTokenKind::Identifier &&
        token_.text == "aggregate") {
      token_ = lexer_.next();
      return parseAggregate();
    }
    return PositiveItem{parseAtom(false)};
  }

  AggregateItem parseAggregate() {
    const DatalogToken output = expect(DatalogTokenKind::Identifier);
    if (!isVariable(output.text) || output.text == "_")
      fail(output, "aggregate output must be a named variable");
    expect(DatalogTokenKind::Equal);
    const DatalogToken operation = expect(DatalogTokenKind::Identifier);
    if (operation.text != "count" && operation.text != "sum" &&
        operation.text != "min" && operation.text != "max" &&
        operation.text != "mean") {
      fail(operation, "unsupported aggregate '" + operation.text + "'");
    }

    std::optional<Expression> projection;
    if (operation.text == "count") {
      if (accept(DatalogTokenKind::LParen))
        expect(DatalogTokenKind::RParen);
    } else {
      expect(DatalogTokenKind::LParen);
      projection = parseExpression();
      expect(DatalogTokenKind::RParen);
    }
    expect(DatalogTokenKind::Colon);
    return {output.text, operation.text, std::move(projection),
            parseAtom(false), location(output)};
  }

  Atom parseAtom(bool allow_expressions) {
    Atom atom;
    const DatalogToken relation = expect(DatalogTokenKind::Identifier);
    atom.relation = relation.text;
    atom.location = location(relation);
    if (!accept(DatalogTokenKind::LParen))
      return atom;
    if (!accept(DatalogTokenKind::RParen)) {
      do {
        atom.arguments.push_back(allow_expressions ? parseExpression()
                                                   : parseBodyTerm());
      } while (accept(DatalogTokenKind::Comma));
      expect(DatalogTokenKind::RParen);
    }
    return atom;
  }

  Expression parseBodyTerm() {
    Expression expression = parseUnary();
    if (expression.kind != Expression::Kind::Variable &&
        expression.kind != Expression::Kind::Constant) {
      fail(token_, "body atoms accept only variables, wildcards, or constants");
    }
    return expression;
  }

  Expression parseExpression() { return parseOr(); }

  Expression parseOr() {
    Expression result = parseAnd();
    while (accept(DatalogTokenKind::OrOr))
      result = binary("||", std::move(result), parseAnd());
    return result;
  }

  Expression parseAnd() {
    Expression result = parseComparison();
    while (accept(DatalogTokenKind::AndAnd))
      result = binary("&&", std::move(result), parseComparison());
    return result;
  }

  Expression parseComparison() {
    Expression result = parseAdditive();
    while (true) {
      std::string operation;
      if (accept(DatalogTokenKind::EqualEqual))
        operation = "==";
      else if (accept(DatalogTokenKind::NotEqual))
        operation = "!=";
      else if (accept(DatalogTokenKind::Less))
        operation = "<";
      else if (accept(DatalogTokenKind::LessEqual))
        operation = "<=";
      else if (accept(DatalogTokenKind::Greater))
        operation = ">";
      else if (accept(DatalogTokenKind::GreaterEqual))
        operation = ">=";
      else
        break;
      result = binary(std::move(operation), std::move(result), parseAdditive());
    }
    return result;
  }

  Expression parseAdditive() {
    Expression result = parseMultiplicative();
    while (true) {
      if (accept(DatalogTokenKind::Plus))
        result = binary("+", std::move(result), parseMultiplicative());
      else if (accept(DatalogTokenKind::Minus))
        result = binary("-", std::move(result), parseMultiplicative());
      else
        break;
    }
    return result;
  }

  Expression parseMultiplicative() {
    Expression result = parseUnary();
    while (true) {
      if (accept(DatalogTokenKind::Star))
        result = binary("*", std::move(result), parseUnary());
      else if (accept(DatalogTokenKind::Slash))
        result = binary("/", std::move(result), parseUnary());
      else if (accept(DatalogTokenKind::Percent))
        result = binary("%", std::move(result), parseUnary());
      else
        break;
    }
    return result;
  }

  Expression parseUnary() {
    if (token_.kind == DatalogTokenKind::Bang) {
      const DatalogToken operation = token_;
      accept(DatalogTokenKind::Bang);
      return unary("!", parseUnary(), location(operation));
    }
    if (token_.kind == DatalogTokenKind::Plus) {
      const DatalogToken operation = token_;
      accept(DatalogTokenKind::Plus);
      return unary("unary+", parseUnary(), location(operation));
    }
    if (token_.kind == DatalogTokenKind::Minus) {
      const DatalogToken operation = token_;
      accept(DatalogTokenKind::Minus);
      if (token_.kind == DatalogTokenKind::Integer) {
        const DatalogToken magnitude = token_;
        token_ = lexer_.next();
        try {
          const std::uint64_t value = std::stoull(magnitude.text);
          const std::uint64_t limit =
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()) +
              1;
          if (value > limit)
            fail(magnitude, "integer literal is outside i64 range");
          if (value == limit)
            return constant(std::numeric_limits<std::int64_t>::min(),
                            location(magnitude));
          return constant(-static_cast<std::int64_t>(value),
                          location(magnitude));
        } catch (const std::invalid_argument &) {
          fail(magnitude, "invalid integer literal");
        } catch (const std::out_of_range &) {
          fail(magnitude, "integer literal is outside i64 range");
        }
      }
      Expression operand = parseUnary();
      if (operand.kind == Expression::Kind::Constant) {
        if (auto *number = std::get_if<double>(&operand.constant.value)) {
          *number = -*number;
          return operand;
        }
      }
      return unary("unary-", std::move(operand), location(operation));
    }
    return parsePrimary();
  }

  Expression parsePrimary() {
    const DatalogToken current = token_;
    if (accept(DatalogTokenKind::Identifier)) {
      if (current.text == "true" || current.text == "false")
        return constant(current.text == "true", location(current));
      if (current.text == "min_lattice" || current.text == "max_lattice" ||
          current.text == "set_lattice") {
        expect(DatalogTokenKind::LParen);
        Expression operand = parseExpression();
        expect(DatalogTokenKind::RParen);
        return unary(current.text, std::move(operand), location(current));
      }
      if (!isVariable(current.text))
        fail(current, "bare symbols are not supported; quote string values");
      return variable(current.text, location(current));
    }
    if (accept(DatalogTokenKind::Integer)) {
      try {
        const std::uint64_t value = std::stoull(current.text);
        if (value <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
          return constant(static_cast<std::int64_t>(value), location(current));
        return constant(value, location(current));
      } catch (const std::exception &) {
        fail(current, "integer literal is outside u64 range");
      }
    }
    if (accept(DatalogTokenKind::Float)) {
      try {
        return constant(std::stod(current.text), location(current));
      } catch (const std::exception &) {
        fail(current, "invalid floating-point literal");
      }
    }
    if (accept(DatalogTokenKind::String))
      return constant(current.text, location(current));
    if (accept(DatalogTokenKind::LParen)) {
      Expression result = parseExpression();
      expect(DatalogTokenKind::RParen);
      return result;
    }
    if (accept(DatalogTokenKind::LBracket)) {
      Scalar::IntegerSet values;
      if (!accept(DatalogTokenKind::RBracket)) {
        do {
          Expression element = parseUnary();
          if (element.kind != Expression::Kind::Constant ||
              !std::holds_alternative<std::int64_t>(element.constant.value)) {
            fail(current, "set constants contain only i64 values");
          }
          values.push_back(std::get<std::int64_t>(element.constant.value));
        } while (accept(DatalogTokenKind::Comma));
        expect(DatalogTokenKind::RBracket);
      }
      return constant(std::move(values), location(current));
    }
    fail(current, "expected an expression");
  }

  static bool isVariable(StringRef name) {
    return name == "_" ||
           (!name.empty() &&
            std::isupper(static_cast<unsigned char>(name.front())));
  }

  template <typename T>
  static Expression constant(T value, SourceLocation location = {}) {
    Expression result;
    result.kind = Expression::Kind::Constant;
    result.constant = Scalar{std::move(value)};
    result.location = std::move(location);
    return result;
  }

  static Expression variable(std::string name, SourceLocation location = {}) {
    Expression result;
    result.kind = Expression::Kind::Variable;
    result.name = std::move(name);
    result.location = std::move(location);
    return result;
  }

  static Expression unary(std::string operation, Expression operand,
                          SourceLocation location = {}) {
    Expression result;
    result.kind = Expression::Kind::Unary;
    result.name = std::move(operation);
    result.operands.push_back(std::move(operand));
    result.location = std::move(location);
    return result;
  }

  static Expression binary(std::string operation, Expression lhs,
                           Expression rhs) {
    Expression result;
    result.kind = Expression::Kind::Binary;
    result.name = std::move(operation);
    result.operands.push_back(std::move(lhs));
    result.operands.push_back(std::move(rhs));
    result.location = result.operands.front().location;
    return result;
  }

  std::string source_;
  DatalogLexer lexer_;
  DatalogToken token_;
  FrontendIR program_;
};

} // namespace

std::string translateDatalogToJson(StringRef input) {
  return translateDatalogToJson(SourceUnit{"<datalog>", input});
}

std::string translateDatalogToJson(llvm::ArrayRef<SourceUnit> inputs) {
  if (inputs.empty())
    throw std::invalid_argument("no Datalog input sources were provided");
  return internal::toJson(internal::parseDatalog(inputs));
}

internal::FrontendIR internal::parseDatalog(
    llvm::ArrayRef<SourceUnit> inputs, const SourceResolver &resolver) {
  if (inputs.empty())
    throw std::invalid_argument("no Datalog input sources were provided");
  FrontendIR program;
  std::unordered_set<std::string> active;
  std::unordered_set<std::string> imported;
  std::function<void(SourceUnit, bool)> parse_source =
      [&](SourceUnit input, bool is_import) {
        const std::string name = input.name.empty() ? "<datalog>"
                                                    : input.name.str();
        if (is_import && imported.count(name))
          return;
        if (!active.insert(name).second) {
          throw FrontendError("include_cycle", name, 1, 1,
                              "cyclic Datalog include");
        }

        FrontendIR parsed = DatalogParser(input).parse();
        std::vector<Include> includes = std::move(parsed.includes);
        parsed.includes.clear();
        program.append(std::move(parsed));
        for (const Include &include : includes) {
          if (!resolver) {
            throw FrontendError(
                "include_requires_resolver", include.location.source,
                include.location.line, include.location.column,
                ".include requires a SourceResolver");
          }
          std::optional<OwnedSourceUnit> resolved =
              resolver(name, include.path);
          if (!resolved) {
            throw FrontendError("include_not_found", include.location.source,
                                include.location.line,
                                include.location.column,
                                "cannot resolve include '" + include.path +
                                    "'");
          }
          const SourceUnit child{resolved->name, resolved->content};
          parse_source(child, true);
        }
        active.erase(name);
        if (is_import)
          imported.insert(name);
      };
  for (const SourceUnit &input : inputs)
    parse_source(input, false);
  return program;
}

} // namespace lotus::datalog::frontend
