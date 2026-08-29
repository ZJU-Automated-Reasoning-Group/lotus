#pragma once

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <cstdint>
#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

enum class ConstantPropagationTag {
  Top,
  Const,
  Bottom,
};

struct ConstantPropagationValue {
  ConstantPropagationTag Tag = ConstantPropagationTag::Top;
  int64_t ConstValue = 0;

  bool operator==(const ConstantPropagationValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

using ConstantPropagationMap =
    std::unordered_map<const llvm::Value *, ConstantPropagationValue>;

struct ConstantPropagationDomain
    : LLVMMonoAnalysisTypes<ConstantPropagationMap> {};

} // namespace mono
