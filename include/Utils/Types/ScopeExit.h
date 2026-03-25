/**
 * @file ScopeExit.h
 * @brief Scope exit guard utilities for RAII-like cleanup
 *
 * This file provides utilities for performing actions when a scope exits,
 * similar to C++20's std::scope_exit. It is based on the presentation:
 * CppCon 2015: Andrei Alexandrescu - "Declarative Control Flow"
 * (https://www.youtube.com/watch?v=WjTrfoiB0MQ).
 *
 * The code was originally developed by Avast Software and is licensed under
 * MIT.
 */

#ifndef RETDEC_UTILS_SCOPE_EXIT_H
#define RETDEC_UTILS_SCOPE_EXIT_H

#include <utility>

// The _IMPL macro is needed to force the expansion of s1 and s2.
#define SCOPE_EXIT_CONCATENATE_IMPL(s1, s2) s1##s2
#define SCOPE_EXIT_CONCATENATE(s1, s2) SCOPE_EXIT_CONCATENATE_IMPL(s1, s2)

#define SCOPE_EXIT_ANONYMOUS_VARIABLE                                          \
  SCOPE_EXIT_CONCATENATE(SCOPE_EXIT_ANONYMOUS_VARIABLE_, __LINE__)

/**
 * @brief Macro for performing actions when the current block exits.
 *
 * Usage:
 * @code
 * SCOPE_EXIT {
 *     stmt1;
 *     stmt2;
 *     ...
 * };
 * @endcode
 * <b>Important:</b> Do not forget the trailing semicolon!
 *
 * The above statements are executed when the current block exits, either
 * normally or via an exception. All variables from outer blocks are
 * automatically captured by reference.
 *
 * This macro is useful for performing automatic cleanup actions, mainly when
 * there is no RAII support.
 *
 * @par Example:
 * @code
 * void processFile(const char* path) {
 *     FILE* f = fopen(path, "r");
 *     SCOPE_EXIT { fclose(f); };
 *
 *     // ... use the file ...
 * } // fclose is automatically called here
 * @endcode
 */
#define SCOPE_EXIT                                                             \
  const auto SCOPE_EXIT_ANONYMOUS_VARIABLE =                                   \
      retdec::utils::ScopeExitGuardHelper() + [&]()

namespace lotus {

//===----------------------------------------------------------------------===//
/// @brief Scope exit guard implementation
///
/// ScopeExitGuard calls the provided function in its destructor, enabling
/// RAII-like cleanup semantics without creating a dedicated RAII type.
///
/// @tparam Function The type of the callable to invoke on destruction
//===----------------------------------------------------------------------===//
template <typename Function> class ScopeExitGuard {
public:
  /// @brief Construct a guard with the given function
  /// @param f The function to call on destruction
  ScopeExitGuard(Function &&f) : f(std::forward<Function>(f)) {}

  /// @brief Destructor calls the stored function
  ~ScopeExitGuard() { f(); }

private:
  Function f;
};

/// @brief Helper type enabling the SCOPE_EXIT macro syntax
struct ScopeExitGuardHelper {};

/// @brief Operator+ enables the SCOPE_EXIT macro syntax
/// @tparam Function The type of the callable
/// @param unused Helper parameter (unused)
/// @param f The function to wrap
/// @return A ScopeExitGuard that will call f on destruction
template <typename Function>
auto operator+(ScopeExitGuardHelper /* unused */, Function &&f) {
  return ScopeExitGuard<Function>(std::forward<Function>(f));
}

} // namespace lotus

#endif
