#pragma once

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <cstdint>
#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

enum class FullConstantTag {
  Bottom,
  Const,
  Top,
};

struct FullConstantValue {
  FullConstantTag Tag;
  int64_t ConstValue;

  static FullConstantValue bottom() { return {FullConstantTag::Bottom, 0}; }
  static FullConstantValue top() { return {FullConstantTag::Top, 0}; }
  static FullConstantValue constant(int64_t Value) {
    return {FullConstantTag::Const, Value};
  }

  bool operator==(const FullConstantValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

struct FullConstantPropagationState {
  bool Unreachable = true;
  std::unordered_map<const llvm::Value *, FullConstantValue> Values;

  bool empty() const { return Unreachable; }
};

struct FullConstantPropagationDomain
    : LLVMMonoAnalysisTypes<FullConstantPropagationState> {};

} // namespace mono
