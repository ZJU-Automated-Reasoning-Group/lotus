#pragma once

#include "Annotation/Pointer/PointerEffectSummary.h"

#include <string>
#include <unordered_map>

namespace llvm {
class StringRef;
} // namespace llvm

namespace annotation {

class ExternalPointerTable {
private:
  using MapType = std::unordered_map<std::string, PointerEffectSummary>;
  MapType table;

  static ExternalPointerTable buildTable(const llvm::StringRef &);
  /// Internal helper: builds the table and reports success via ok.
  /// Declared as a static member so it can access the private `table` field.
  static ExternalPointerTable buildTableImpl(const llvm::StringRef &, bool &ok);

public:
  using const_iterator = MapType::const_iterator;

  const PointerEffectSummary *lookup(const llvm::StringRef &name) const;

  // Note: this function should be used for testing only. The only sensible way
  // of constructing an external table is calling loadFromFile()
  void addEffect(const llvm::StringRef &name, PointerEffect &&e);

  const_iterator begin() const { return table.begin(); }
  const_iterator end() const { return table.end(); }
  size_t size() const { return table.size(); }

  /// Load from file; calls std::exit(-1) on parse error (original behaviour).
  static ExternalPointerTable loadFromFile(const char *fileName);

  /// Load from file without calling std::exit().
  /// Returns true on success; outTable is populated only on success.
  static bool loadFromFile(const char *fileName,
                           ExternalPointerTable &outTable);
};

} // namespace annotation
