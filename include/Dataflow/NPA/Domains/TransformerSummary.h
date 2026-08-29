#ifndef NPA_TRANSFORMER_SUMMARY_H
#define NPA_TRANSFORMER_SUMMARY_H

#include "Dataflow/NPA/Core/Domain.h"

#include <set>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace npa {

template <class Op, class OpLess = std::less<Op>> struct TransformerLess {
  bool operator()(const std::vector<Op> &lhs, const std::vector<Op> &rhs) const {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end(), OpLess{});
  }
};

template <class Op, class OpLess = std::less<Op>>
struct TransformerSummaryValue {
  using transformer_type = std::vector<Op>;
  std::set<transformer_type, TransformerLess<Op, OpLess>> transformers;
  bool overflow = false;
  std::unordered_set<const void *> may_write;

  bool operator==(const TransformerSummaryValue &other) const {
    return overflow == other.overflow && transformers == other.transformers &&
           may_write == other.may_write;
  }
};

/// Generic abstract-summary domain for subdistributive analyses.
///
/// The current implementation keeps a finite set of summary transformers and an
/// overflow bit, which preserves the existing CP/IA behavior while decoupling
/// them from the older PathTransferSummary-specific API.
template <class Op, class OpLess = std::less<Op>>
class TransformerSummary {
public:
  using value_type = TransformerSummaryValue<Op, OpLess>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr std::size_t max_transformers = 4096;
  static constexpr std::size_t max_transformer_length = 320;

  static value_type zero() { return {}; }

  static value_type one() {
    value_type out;
    out.transformers.insert(typename value_type::transformer_type{});
    return out;
  }

  static value_type singleton(const Op &op) {
    value_type out;
    insertTransformer(out, typename value_type::transformer_type{op});
    return out;
  }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }

  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.overflow = a.overflow || b.overflow;
    out.may_write.insert(b.may_write.begin(), b.may_write.end());
    for (const auto &transformer : b.transformers)
      insertTransformer(out, transformer);
    return out;
  }

  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  static value_type extend(const value_type &a, const value_type &b) {
    if ((a.transformers.empty() && !a.overflow) ||
        (b.transformers.empty() && !b.overflow))
      return zero();
    value_type out;
    out.overflow = a.overflow || b.overflow;
    out.may_write.insert(a.may_write.begin(), a.may_write.end());
    out.may_write.insert(b.may_write.begin(), b.may_write.end());
    for (const auto &inner : b.transformers) {
      for (const auto &outer : a.transformers) {
        typename value_type::transformer_type composed;
        composed.reserve(inner.size() + outer.size());
        composed.insert(composed.end(), inner.begin(), inner.end());
        composed.insert(composed.end(), outer.begin(), outer.end());
        insertTransformer(out, std::move(composed));
      }
    }
    return out;
  }

  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }

  static value_type subtract(const value_type &a, const value_type &) {
    return a;
  }

private:
  template <typename... Ts> using void_t = void;

  template <typename T, typename = void> struct HasPointerDest : std::false_type {};

  template <typename T>
  struct HasPointerDest<T, void_t<decltype(std::declval<T>().dest)>>
      : std::integral_constant<bool,
                               std::is_pointer<decltype(std::declval<T>().dest)>::value> {};

  template <typename T = Op>
  static typename std::enable_if<HasPointerDest<T>::value, void>::type
  noteWrite(value_type &out, const T &op) {
    if (op.dest)
      out.may_write.insert(op.dest);
  }

  template <typename T = Op>
  static typename std::enable_if<!HasPointerDest<T>::value, void>::type
  noteWrite(value_type &, const T &) {}

  template <typename T, typename = void>
  struct HasSummaryCanBeOverwritten : std::false_type {};

  template <typename T>
  struct HasSummaryCanBeOverwritten<
      T, void_t<decltype(std::declval<const T &>().summaryCanBeOverwritten())>>
      : std::true_type {};

  template <typename T, typename = void>
  struct HasSummaryCanOverwritePrevious : std::false_type {};

  template <typename T>
  struct HasSummaryCanOverwritePrevious<
      T, void_t<decltype(std::declval<const T &>().summaryCanOverwritePrevious())>>
      : std::true_type {};

  static typename value_type::transformer_type
  canonicalizeTransformer(typename value_type::transformer_type transformer) {
    typename value_type::transformer_type out;
    out.reserve(transformer.size());

    for (const auto &op : transformer) {
      if (!out.empty() && canFoldTrailingWrite(out.back(), op)) {
        out.back() = op;
      } else {
        out.push_back(op);
      }
    }
    return out;
  }

  template <typename T = Op>
  static typename std::enable_if<HasPointerDest<T>::value, bool>::type
  canFoldTrailingWrite(const T &previous, const T &current) {
    return previous.dest && previous.dest == current.dest &&
           summaryCanBeOverwritten(previous) &&
           summaryCanOverwritePrevious(current);
  }

  template <typename T = Op>
  static typename std::enable_if<!HasPointerDest<T>::value, bool>::type
  canFoldTrailingWrite(const T &, const T &) {
    return false;
  }

  template <typename T = Op>
  static typename std::enable_if<HasSummaryCanBeOverwritten<T>::value, bool>::type
  summaryCanBeOverwritten(const T &op) {
    return op.summaryCanBeOverwritten();
  }

  template <typename T = Op>
  static typename std::enable_if<!HasSummaryCanBeOverwritten<T>::value, bool>::type
  summaryCanBeOverwritten(const T &) {
    return false;
  }

  template <typename T = Op>
  static typename std::enable_if<HasSummaryCanOverwritePrevious<T>::value, bool>::type
  summaryCanOverwritePrevious(const T &op) {
    return op.summaryCanOverwritePrevious();
  }

  template <typename T = Op>
  static typename std::enable_if<!HasSummaryCanOverwritePrevious<T>::value, bool>::type
  summaryCanOverwritePrevious(const T &) {
    return false;
  }

  static void insertTransformer(value_type &out,
                                typename value_type::transformer_type transformer) {
    transformer = canonicalizeTransformer(std::move(transformer));
    for (const auto &op : transformer)
      noteWrite(out, op);
    if (transformer.size() > max_transformer_length) {
      out.overflow = true;
      return;
    }
    if (out.transformers.size() >= max_transformers &&
        !out.transformers.count(transformer)) {
      out.overflow = true;
      return;
    }
    out.transformers.insert(std::move(transformer));
  }
};

} // namespace npa

#endif // NPA_TRANSFORMER_SUMMARY_H
