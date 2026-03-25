#ifndef SUPPORT_DEMANGLE_H
#define SUPPORT_DEMANGLE_H

#include <string>

#include <llvm/Demangle/Demangle.h>

namespace DemangleUtils {

/**
 * Demangle a C++ or Rust symbol name to make it more readable.
 * Handles both C++ mangled names (e.g., _Z...) and Rust mangled names.
 *
 * @param mangledName The mangled symbol name
 * @return The demangled name, or the original name if demangling fails
 */
std::string demangle(const std::string &mangledName);

/**
 * Check if a name appears to be mangled (C++ or Rust style).
 *
 * @param name The name to check
 * @return true if the name appears to be mangled, false otherwise
 */
bool isMangled(const std::string &name);

/**
 * Demangle with additional cleanup for Rust-specific artifacts.
 * Removes hash suffixes and converts special encodings.
 *
 * @param mangledName The mangled symbol name
 * @return The cleaned and demangled name
 */
std::string demangleWithCleanup(const std::string &mangledName);

} // namespace DemangleUtils

#endif // SUPPORT_DEMANGLE_H
