#pragma once

#include "Annotation/ModRef/ModRefEffectSummary.h"

#include <string>
#include <unordered_map>

namespace llvm {
class StringRef;
} // namespace llvm

namespace annotation {

class ExternalModRefTable {
private:
  using MapType = std::unordered_map<std::string, ModRefEffectSummary>;
  MapType table;

  static ExternalModRefTable buildTable(const llvm::StringRef &);
  /// Internal helper: builds the table and reports success via ok.
  /// Declared as a static member so it can access the private `table` field.
  static ExternalModRefTable buildTableImpl(const llvm::StringRef &, bool &ok);

public:
  using const_iterator = MapType::const_iterator;

  ExternalModRefTable() = default;

  const ModRefEffectSummary *lookup(const llvm::StringRef &) const;

  const_iterator begin() const { return table.begin(); }
  const_iterator end() const { return table.end(); }
  size_t size() const { return table.size(); }

  /// Load from file; calls std::exit(-1) on parse error (original behaviour).
  static ExternalModRefTable loadFromFile(const char *fileName);

  /// Load from file without calling std::exit().
  /// Returns true on success; outTable is populated only on success.
  static bool loadFromFile(const char *fileName, ExternalModRefTable &outTable);
};

} // namespace annotation
