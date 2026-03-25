//===-- PathExpressions/Regex.h - Regular expressions for path labels ---===//
//
// Migrated from Ultimate Library-PathExpressions (v0.3.1).
// Original Java package:
// de.uni_freiburg.informatik.ultimate.lib.pathexpressions.regex
//
// Provides the regex AST, structural equality, a visitor interface, Tarjan's
// simplification operators, and faithful string renderings.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEX_H
#define LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEX_H

#include <cstddef>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace lotus {
namespace pathexpressions {

template <typename L> class Union;
template <typename L> class Concatenation;
template <typename L> class Star;
template <typename L> class Literal;
template <typename L> class Epsilon;
template <typename L> class EmptySet;

template <typename L, typename RET, typename ARG> struct IRegexVisitor {
  virtual ~IRegexVisitor() = default;
  virtual RET visit(const Union<L> &re, ARG arg) = 0;
  virtual RET visit(const Concatenation<L> &re, ARG arg) = 0;
  virtual RET visit(const Star<L> &re, ARG arg) = 0;
  virtual RET visit(const Literal<L> &re, ARG arg) = 0;
  virtual RET visit(const Epsilon<L> &re, ARG arg) = 0;
  virtual RET visit(const EmptySet<L> &re, ARG arg) = 0;
};

template <typename L> class IRegex {
public:
  virtual ~IRegex() = default;

  virtual bool isEpsilon() const { return false; }
  virtual bool isEmptySet() const { return false; }

  virtual std::string toString() const = 0;
  virtual std::size_t hashCode() const = 0;
  virtual bool equals(const IRegex<L> &other) const = 0;

  template <typename RET, typename ARG>
  RET accept(IRegexVisitor<L, RET, ARG> &visitor, ARG arg) const {
    if (auto p = dynamic_cast<const Union<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    if (auto p = dynamic_cast<const Concatenation<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    if (auto p = dynamic_cast<const Star<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    if (auto p = dynamic_cast<const Literal<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    if (auto p = dynamic_cast<const Epsilon<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    if (auto p = dynamic_cast<const EmptySet<L> *>(this)) {
      return visitor.visit(*p, arg);
    }
    // All concrete regexes must be one of the above.
    throw std::logic_error("Unknown IRegex dynamic type");
  }

  template <typename RET>
  RET accept(IRegexVisitor<L, RET, std::nullptr_t> &visitor) const {
    return accept(visitor, nullptr);
  }

  template <typename RET, typename ARG>
  std::enable_if_t<std::is_pointer<ARG>::value, RET>
  accept(IRegexVisitor<L, RET, ARG> &visitor) const {
    return accept(visitor, static_cast<ARG>(nullptr));
  }
};

template <typename L> using RegexRef = std::shared_ptr<const IRegex<L>>;

namespace detail {
inline std::size_t hashCombine(const std::size_t seed,
                               const std::size_t value) {
  // Similar spirit to Java's Objects.hash (not identical).
  return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

template <typename T> inline std::string toStringViaStream(const T &value) {
  std::ostringstream oss;
  oss << value;
  return oss.str();
}
} // namespace detail

template <typename L> class Epsilon final : public IRegex<L> {
public:
  bool isEpsilon() const override { return true; }
  std::string toString() const override { return "ε"; }
  std::size_t hashCode() const override { return 0xE11A; }
  bool equals(const IRegex<L> &other) const override {
    return dynamic_cast<const Epsilon<L> *>(&other) != nullptr;
  }
};

template <typename L> class EmptySet final : public IRegex<L> {
public:
  bool isEmptySet() const override { return true; }
  std::string toString() const override { return "∅"; }
  std::size_t hashCode() const override { return 0xE570; }
  bool equals(const IRegex<L> &other) const override {
    return dynamic_cast<const EmptySet<L> *>(&other) != nullptr;
  }
};

template <typename L> class Literal final : public IRegex<L> {
public:
  explicit Literal(L letter) : letter_(std::move(letter)) {}

  const L &getLetter() const { return letter_; }

  std::string toString() const override {
    return detail::toStringViaStream(letter_);
  }
  std::size_t hashCode() const override { return std::hash<L>()(letter_); }
  bool equals(const IRegex<L> &other) const override {
    const auto *o = dynamic_cast<const Literal<L> *>(&other);
    return o != nullptr && letter_ == o->letter_;
  }

private:
  L letter_;
};

template <typename L> class Union final : public IRegex<L> {
public:
  Union(RegexRef<L> first, RegexRef<L> second)
      : first_(std::move(first)), second_(std::move(second)) {}

  const RegexRef<L> &getFirst() const { return first_; }
  const RegexRef<L> &getSecond() const { return second_; }

  std::string toString() const override {
    return "{" + first_->toString() + " ∪ " + second_->toString() + "}";
  }
  std::size_t hashCode() const override {
    return detail::hashCombine(first_->hashCode(), second_->hashCode());
  }
  bool equals(const IRegex<L> &other) const override {
    const auto *o = dynamic_cast<const Union<L> *>(&other);
    return o != nullptr && first_->equals(*o->first_) &&
           second_->equals(*o->second_);
  }

private:
  RegexRef<L> first_;
  RegexRef<L> second_;
};

template <typename L> class Concatenation final : public IRegex<L> {
public:
  Concatenation(RegexRef<L> first, RegexRef<L> second)
      : first_(std::move(first)), second_(std::move(second)) {}

  const RegexRef<L> &getFirst() const { return first_; }
  const RegexRef<L> &getSecond() const { return second_; }

  std::string toString() const override {
    return "(" + first_->toString() + "·" + second_->toString() + ")";
  }
  std::size_t hashCode() const override {
    return detail::hashCombine(first_->hashCode(), second_->hashCode());
  }
  bool equals(const IRegex<L> &other) const override {
    const auto *o = dynamic_cast<const Concatenation<L> *>(&other);
    return o != nullptr && first_->equals(*o->first_) &&
           second_->equals(*o->second_);
  }

private:
  RegexRef<L> first_;
  RegexRef<L> second_;
};

template <typename L> class Star final : public IRegex<L> {
public:
  explicit Star(RegexRef<L> inner) : inner_(std::move(inner)) {}

  const RegexRef<L> &getInner() const { return inner_; }

  std::string toString() const override {
    return "[" + inner_->toString() + "]* ";
  }
  std::size_t hashCode() const override { return 257u * inner_->hashCode(); }
  bool equals(const IRegex<L> &other) const override {
    const auto *o = dynamic_cast<const Star<L> *>(&other);
    return o != nullptr && inner_->equals(*o->inner_);
  }

private:
  RegexRef<L> inner_;
};

template <typename L> struct Regex {
  static RegexRef<L> union_(RegexRef<L> a, RegexRef<L> b) {
    return std::make_shared<Union<L>>(std::move(a), std::move(b));
  }

  static RegexRef<L> concat(RegexRef<L> a, RegexRef<L> b) {
    return std::make_shared<Concatenation<L>>(std::move(a), std::move(b));
  }

  static RegexRef<L> star(RegexRef<L> a) {
    return std::make_shared<Star<L>>(std::move(a));
  }

  static RegexRef<L> literal(L letter) {
    return std::make_shared<Literal<L>>(std::move(letter));
  }

  static RegexRef<L> epsilon() {
    static RegexRef<L> instance = std::make_shared<Epsilon<L>>();
    return instance;
  }

  static RegexRef<L> emptySet() {
    static RegexRef<L> instance = std::make_shared<EmptySet<L>>();
    return instance;
  }

  static RegexRef<L> simplifiedUnion(RegexRef<L> a, RegexRef<L> b) {
    if (a->isEmptySet()) {
      return b;
    }
    if (b->isEmptySet()) {
      return a;
    }
    // Not part of Tarjan's simplification operator "[R]" but present in
    // Ultimate.
    if (a->equals(*b)) {
      return a;
    }
    return union_(std::move(a), std::move(b));
  }

  static RegexRef<L> simplifiedConcatenation(RegexRef<L> a, RegexRef<L> b) {
    if (a->isEmptySet() || b->isEmptySet()) {
      return emptySet();
    }
    if (a->isEpsilon()) {
      return b;
    }
    if (b->isEpsilon()) {
      return a;
    }
    return concat(std::move(a), std::move(b));
  }

  static RegexRef<L> simplifiedStar(RegexRef<L> reg) {
    if (reg->isEmptySet() || reg->isEpsilon()) {
      return epsilon();
    }
    return star(std::move(reg));
  }
};

} // namespace pathexpressions
} // namespace lotus

#endif // LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEX_H
