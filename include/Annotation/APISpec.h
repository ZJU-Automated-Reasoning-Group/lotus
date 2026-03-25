// Unified API specifications loader for pointer, mod/ref, and related specs
// This component parses spec files (e.g., config/ptr.spec, config/modref.spec)
// into a structured representation consumable by different analyses.

#ifndef LOTUS_SUPPORT_APISPEC_H
#define LOTUS_SUPPORT_APISPEC_H

// Avoid including LLVM headers in the public header to keep dependencies light.
namespace llvm {
class Function;
class Module;
} // namespace llvm

#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {

// Operation kinds supported across specs
enum class SpecOpKind {
  Ignore,  // IGNORE / no effect
  Alloc,   // ALLOC
  Dealloc, // DEALLOC
  Copy,    // COPY
  Exit,    // EXIT
  Mod,     // MOD (modifies memory)
  Ref      // REF (reads/references memory)
};

// Qualifiers as found in specs (we preserve them as-is, but also expose
// well-known ones via accessors)
enum class QualifierKind {
  Value,  // V (value/pointer itself)
  Region, // R (pointee region)
  Data,   // D (data)
  Unknown // anything else (including NULL/STATIC handled via selector)
};

// Where a value comes from (e.g., return value or N-th argument)
enum class SelectorKind {
  Ret,      // Ret
  Arg,      // Arg<N>
  AfterArg, // AfterArg<N> (position after argument N)
  Static,   // STATIC (external static address)
  Null      // NULL (null pointer literal)
};

struct ValueSelector {
  SelectorKind kind;
  // index is used for Arg/AfterArg; ignored otherwise
  int index;
  bool isValid;
};

inline bool operator==(const ValueSelector &a, const ValueSelector &b) {
  return a.kind == b.kind && a.index == b.index && a.isValid == b.isValid;
}

struct CopyEffect {
  ValueSelector dst;
  QualifierKind dstQualifier;
  ValueSelector src;
  QualifierKind srcQualifier;
};

struct AllocEffect {
  // Optional argument index that influences allocation size (if provided by
  // spec)
  int sizeArgIndex; // -1 if unspecified
};

struct ModRefEffect {
  // For MOD/REF records in modref.spec
  SpecOpKind op; // Mod or Ref
  ValueSelector target;
  QualifierKind qualifier;
};

// All effects for a single function (aggregated from possibly multiple lines)
struct FunctionSpec {
  std::string functionName;
  bool isIgnored{false};
  bool isExit{false};
  bool isAllocator{false};
  bool isDeallocator{false};
  std::vector<AllocEffect> allocs; // may contain multiple variants
  std::vector<CopyEffect> copies;
  std::vector<ModRefEffect> modref; // entries from modref.spec
};

// APISpec contains specifications across all functions and ops loaded from one
// or more files. It also provides query helpers for consumers.
class APISpec {
public:
  APISpec() = default;

  // Load a spec file. Multiple files can be loaded; rules will be merged per
  // function. Later-loaded files can extend or override earlier state.
  // Returns true on success; false on I/O error (parsing continues
  // best-effort).
  bool loadFile(const std::string &path, std::string &errorMessage);

  // Convenience: load many files. Returns first error message if any.
  bool loadFiles(const std::vector<std::string> &paths,
                 std::string &errorMessage);

  // Expose raw map for advanced consumers
  const std::unordered_map<std::string, FunctionSpec> &all() const {
    return nameToSpec;
  }

  // Lookups by function (by name). If multiple overloads/mangled names exist,
  // pass the exact LLVM function name.
  const FunctionSpec *get(const std::string &functionName) const;

  // Queries
  bool isIgnored(const std::string &functionName) const;
  bool isExitLike(const std::string &functionName) const;
  bool isAllocatorLike(const std::string &functionName) const;
  bool isDeallocatorLike(const std::string &functionName) const;

  // Returns copy effects for function or empty vector if none/unknown.
  std::vector<CopyEffect> getCopies(const std::string &functionName) const;

  // Returns mod/ref effects for function or empty vector if none/unknown.
  std::vector<ModRefEffect> getModRefs(const std::string &functionName) const;

  // Insert or replace a fully constructed FunctionSpec. Useful for custom
  // specs provided by analyses at runtime (e.g., command-line overrides).
  void addOrReplaceSpec(const FunctionSpec &spec);

private:
  std::unordered_map<std::string, FunctionSpec> nameToSpec;

  // Parsing helpers
  static bool parseLine(const std::string &line, std::string &outFunc,
                        SpecOpKind &outOp, std::vector<std::string> &outTokens);

  static ValueSelector parseSelector(const std::string &token);
  static QualifierKind parseQualifier(const std::string &token);

  void applyAlloc(FunctionSpec &spec, const std::vector<std::string> &tokens);
  // Returns false (and emits a warning) if tokens are malformed.
  bool applyCopy(FunctionSpec &spec, const std::vector<std::string> &tokens);
  void applyIgnore(FunctionSpec &spec);
  void applyDealloc(FunctionSpec &spec);
  void applyExit(FunctionSpec &spec);
  // Returns false (and emits a warning) if tokens are malformed.
  bool applyModRef(FunctionSpec &spec, SpecOpKind op,
                   const std::vector<std::string> &tokens);
};

// Utility helpers
inline bool isValueQualifier(QualifierKind q) {
  return q == QualifierKind::Value;
}
inline bool isRegionQualifier(QualifierKind q) {
  return q == QualifierKind::Region;
}
inline bool isDataQualifier(QualifierKind q) {
  return q == QualifierKind::Data;
}

} // namespace lotus

#endif // LOTUS_SUPPORT_APISPEC_H
