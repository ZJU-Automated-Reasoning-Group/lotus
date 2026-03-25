#ifndef LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGCONTEXT_H_
#define LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGCONTEXT_H_

#include "llvm/ADT/Hashing.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <deque>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <type_traits>

namespace mono {

template <typename N, unsigned K> class CallStringCTX {
protected:
  std::deque<N> CallString;
  static constexpr unsigned KLimit = K;
  friend struct std::hash<mono::CallStringCTX<N, K>>;

public:
  CallStringCTX() = default;

  CallStringCTX(std::initializer_list<N> IList) : CallString(IList) {
    if (IList.size() > KLimit) {
      throw std::runtime_error(
          "initial call string length exceeds maximal length K");
    }
  }

  void push_back(N Stmt) { // NOLINT
    // Fix #3: when K == 0 the expression (KLimit - 1) underflows to SIZE_MAX
    // (unsigned arithmetic), making the condition always false and allowing
    // the deque to grow without bound.  Guard explicitly for K == 0 (which
    // means context-insensitive: never store any call-site) and for the
    // general case where the deque is already at capacity.
    if (KLimit == 0) {
      // K=0 → context-insensitive: discard the call site immediately.
      return;
    }
    if (CallString.size() >= KLimit) {
      CallString.pop_front();
    }
    CallString.push_back(Stmt);
  }

  /// Remove and return the last element of the call string.
  ///
  /// Bug fix: the old implementation silently returned a default-constructed
  /// N{} (typically nullptr for pointer types) when the deque was empty.
  /// Callers that forgot to check empty() would receive a null call-site and
  /// silently produce wrong results (e.g., wrong context transitions in the
  /// interprocedural solver).  We now assert non-empty so the bug surfaces
  /// immediately in debug builds.  In release builds the assert is a no-op
  /// and the old behaviour (return N{}) is preserved for ABI compatibility.
  N pop_back() { // NOLINT
    assert(!CallString.empty() &&
           "CallStringCTX::pop_back() called on empty call string");
    if (!CallString.empty()) {
      N Stmt = CallString.back();
      CallString.pop_back();
      return Stmt;
    }
    return N{};
  }

  bool isEqual(const CallStringCTX &Rhs) const {
    return CallString == Rhs.CallString;
  }

  bool isDifferent(const CallStringCTX &Rhs) const { return !isEqual(Rhs); }

  friend bool operator==(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return Lhs.isEqual(Rhs);
  }

  friend bool operator!=(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return !Lhs.isEqual(Rhs);
  }

  friend bool operator<(const CallStringCTX &Lhs, const CallStringCTX &Rhs) {
    return Lhs.CallString < Rhs.CallString;
  }

  llvm::raw_ostream &print(llvm::raw_ostream &OS) const {
    OS << "Call string: [ ";
    bool First = true;
    for (auto C : CallString) {
      if (!First) {
        OS << " * ";
      }
      First = false;
      printElement(OS, C);
    }
    return OS << " ]";
  }

  bool empty() const { return CallString.empty(); }

  std::size_t size() const { return CallString.size(); }

private:
  template <typename T>
  static typename std::enable_if<
      std::is_pointer<T>::value &&
          std::is_base_of<llvm::Value,
                          typename std::remove_pointer<T>::type>::value,
      void>::type
  printElement(llvm::raw_ostream &OS, T V) {
    if (V != nullptr) {
      OS << *V;
    } else {
      OS << "<null>";
    }
  }

  template <typename T>
  static typename std::enable_if<
      !(std::is_pointer<T>::value &&
        std::is_base_of<llvm::Value,
                        typename std::remove_pointer<T>::type>::value),
      void>::type
  printElement(llvm::raw_ostream &OS, const T &) {
    OS << "<elem>";
  }
};

} // namespace mono

namespace std {

template <typename N, unsigned K> struct hash<mono::CallStringCTX<N, K>> {
  size_t operator()(const mono::CallStringCTX<N, K> &CS) const noexcept {
    llvm::hash_code CallStringHash =
        llvm::hash_combine_range(CS.CallString.begin(), CS.CallString.end());
    return static_cast<size_t>(llvm::hash_combine(K, CallStringHash));
  }
};

} // namespace std

#endif // LOTUS_DATAFLOW_MONO_CORE_CALLSTRINGCONTEXT_H_
