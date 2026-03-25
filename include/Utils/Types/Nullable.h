/******************************************************************************
 * Copyright (c) 2023 Fabian Schiebel.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Fabian Schiebel and others
 *****************************************************************************/

//===----------------------------------------------------------------------===//
/// @file Nullable.h
/// @brief Nullable type utilities for optional value handling
///
/// This file provides utilities for handling nullable values. It defines the
/// `Nullable` type alias and `unwrapNullable` functions.
///
/// The `Nullable<T>` type alias behaves as follows:
/// - If T is convertible to bool (like util::Optional<T>), it uses T directly
/// - Otherwise, it wraps T in util::Optional<T>
///
/// This allows seamless interoperability between Optional types and other
/// nullable-like types.
///
///===----------------------------------------------------------------------===//

#ifndef PHASAR_UTILS_NULLABLE_H
#define PHASAR_UTILS_NULLABLE_H

#include "Utils/Types/Optional.h"

#include <type_traits>
#include <utility>

namespace psr {

/// @brief Type alias for nullable values
///
/// If T is convertible to bool, Nullable<T> is just T. Otherwise, it's
/// util::Optional<T>. This allows writing generic code that accepts both
/// optional types and regular types.
///
/// @tparam T The type to make nullable
template <typename T>
using Nullable = std::conditional_t<std::is_convertible<T, bool>::value, T,
                                    util::Optional<T>>;

/// @brief Unwrap a nullable value
///
/// If the value is already a bool-convertible type, returns it unchanged.
/// Otherwise, extracts the value from an Optional.
///
/// @tparam T The underlying type
/// @param Val The nullable value to unwrap
/// @return The unwrapped value
template <typename T>
std::enable_if_t<std::is_convertible<T, bool>::value, T &&>
unwrapNullable(T &&Val) noexcept {
  return std::forward<T>(Val);
}
/// @brief Unwrap an Optional rvalue
/// @tparam T The underlying type
/// @param Val The Optional to unwrap
/// @return The contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, T>
unwrapNullable(util::Optional<T> &&Val) noexcept {
  return *std::move(Val);
}
/// @brief Unwrap an Optional const lvalue
/// @tparam T The underlying type
/// @param Val The Optional to unwrap
/// @return A const reference to the contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, const T &>
unwrapNullable(const util::Optional<T> &Val) noexcept {
  return *Val;
}
/// @brief Unwrap an Optional lvalue
/// @tparam T The underlying type
/// @param Val The Optional to unwrap
/// @return A reference to the contained value
template <typename T>
std::enable_if_t<!std::is_convertible<T, bool>::value, T &>
unwrapNullable(util::Optional<T> &Val) noexcept {
  return *Val;
}
} // namespace psr

#endif // PHASAR_UTILS_NULLABLE_H
