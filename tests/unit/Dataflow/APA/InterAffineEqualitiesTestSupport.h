#pragma once

#include "Dataflow/APA/Analyses/Inter/AffineEqualities.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

std::vector<elimination::AffineState> materializedAffineStatesForBlock(
    const std::map<elimination::BlockKey,
                   elimination::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<elimination::AffineState> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(elimination::materializeAffineExpressions(entry.second));
  }
  return out;
}

std::vector<const elimination::AffineRelationDomain::value_type *>
relationsForBlock(
    const std::map<elimination::BlockKey,
                   elimination::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<const elimination::AffineRelationDomain::value_type *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

bool equalityMatchesUpToNegation(
    const elimination::AffineEquality &equality, int64_t constant,
    std::initializer_list<std::pair<const llvm::Value *, int64_t>> terms) {
  auto matches = [&](int sign) {
    if (equality.constant != sign * constant)
      return false;
    if (equality.terms.size() != terms.size())
      return false;
    for (const auto &term : terms) {
      auto It = equality.terms.find(term.first);
      if (It == equality.terms.end() || It->second != sign * term.second)
        return false;
    }
    return true;
  };
  return matches(1) || matches(-1);
}

bool stateHasEquality(
    const elimination::AffineState &state, int64_t constant,
    std::initializer_list<std::pair<const llvm::Value *, int64_t>> terms) {
  return std::any_of(state.equalities.begin(), state.equalities.end(),
                     [&](const elimination::AffineEquality &equality) {
                       return equalityMatchesUpToNegation(equality, constant,
                                                          terms);
                     });
}

int64_t congruenceScale(unsigned modulusBits) {
  unsigned width = elimination::AffineRelationDomain::componentBitWidth();
  return static_cast<int64_t>(uint64_t{1} << (width - modulusBits));
}

} // namespace
