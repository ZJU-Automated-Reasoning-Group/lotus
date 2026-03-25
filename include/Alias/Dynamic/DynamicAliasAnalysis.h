#pragma once

#include "Alias/Dynamic/AliasPair.h"
#include "Alias/Dynamic/DynamicPointer.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>

namespace dynamic {

class DynamicAliasAnalysis {
private:
  using AliasPairSet = llvm::DenseSet<AliasPair>;
  using AnalysisMap = llvm::DenseMap<DynamicPointer, AliasPairSet>;
  AnalysisMap aliasPairMap;

  const char *fileName;

public:
  using const_iterator = AnalysisMap::const_iterator;

  DynamicAliasAnalysis(const char *fileName);

  void runAnalysis();

  const AliasPairSet *getAliasPairs(DynamicPointer) const;

  const_iterator begin() const { return aliasPairMap.begin(); }
  const_iterator end() const { return aliasPairMap.end(); }
};
} // namespace dynamic
