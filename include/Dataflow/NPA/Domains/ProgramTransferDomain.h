#ifndef NPA_PROGRAM_TRANSFER_DOMAIN_H
#define NPA_PROGRAM_TRANSFER_DOMAIN_H

#include "Dataflow/NPA/Core/NPACommon.h"

#include <set>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace npa {

template <class Op, class OpLess = std::less<Op>> struct PathLess {
  bool operator()(const std::vector<Op> &lhs, const std::vector<Op> &rhs) const {
    return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(),
                                        rhs.end(), OpLess{});
  }
};

template <class Op, class OpLess = std::less<Op>> struct ProgramTransfer {
  using path_type = std::vector<Op>;
  std::set<path_type, PathLess<Op, OpLess>> paths;
  bool overflow = false;
  std::unordered_set<const void *> may_write;

  bool operator==(const ProgramTransfer &other) const {
    return overflow == other.overflow && paths == other.paths &&
           may_write == other.may_write;
  }
};

template <class Op, class OpLess = std::less<Op>> class ProgramTransferDomain {
public:
  using value_type = ProgramTransfer<Op, OpLess>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr std::size_t max_paths = 4096;
  static constexpr std::size_t max_path_length = 320;

  static value_type zero() { return {}; }

  static value_type one() {
    value_type out;
    out.paths.insert(typename value_type::path_type{});
    return out;
  }

  static value_type singleton(const Op &op) {
    value_type out;
    insertPath(out, typename value_type::path_type{op});
    return out;
  }

  static bool equal(const value_type &a, const value_type &b) { return a == b; }

  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.overflow = a.overflow || b.overflow;
    out.may_write.insert(b.may_write.begin(), b.may_write.end());
    for (const auto &path : b.paths)
      insertPath(out, path);
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
    if ((a.paths.empty() && !a.overflow) || (b.paths.empty() && !b.overflow))
      return zero();
    value_type out;
    out.overflow = a.overflow || b.overflow;
    out.may_write.insert(a.may_write.begin(), a.may_write.end());
    out.may_write.insert(b.may_write.begin(), b.may_write.end());
    for (const auto &bp : b.paths) {
      for (const auto &ap : a.paths) {
        typename value_type::path_type composed;
        composed.reserve(bp.size() + ap.size());
        composed.insert(composed.end(), bp.begin(), bp.end());
        composed.insert(composed.end(), ap.begin(), ap.end());
        insertPath(out, std::move(composed));
      }
    }
    return out;
  }

  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }

  static value_type subtract(const value_type &a, const value_type & /*b*/) {
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

  static void insertPath(value_type &out, typename value_type::path_type path) {
    for (const auto &op : path)
      noteWrite(out, op);
    if (path.size() > max_path_length) {
      out.overflow = true;
      return;
    }
    if (out.paths.size() >= max_paths && !out.paths.count(path)) {
      out.overflow = true;
      return;
    }
    out.paths.insert(std::move(path));
  }
};

} // namespace npa

#endif // NPA_PROGRAM_TRANSFER_DOMAIN_H
