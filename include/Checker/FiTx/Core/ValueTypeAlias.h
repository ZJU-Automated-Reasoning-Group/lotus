/// \file ValueTypeAlias.h
/// \brief Instruction-level alias mapping: instruction -> operands and
/// store–call alias (which store feeds a call). Used for type/operand tracking;
/// main typestate alias propagation uses AliasValues (store-based, per block).
#pragma once
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Value.h"

#include <vector>

namespace fitx {

struct FunctionSigniture {
  llvm::Type *return_type;
  std::vector<llvm::Type *> argument_type;
};

/// Operand list for a framework instruction (used by ValueTypeAlias).
class Operands {
public:
  Operands() = default;
  Operands(std::vector<std::shared_ptr<fitx::Value>> values);
  Operands(
      std::vector<std::shared_ptr<std::shared_ptr<fitx::Value>>> values);

  std::shared_ptr<std::shared_ptr<fitx::Value>>
  operator[](const int index);

  void add(std::shared_ptr<std::shared_ptr<fitx::Value>> value);
  void add(std::shared_ptr<fitx::Value> value);

  size_t size() const;

private:
  std::vector<std::shared_ptr<std::shared_ptr<fitx::Value>>> values_;
};

/// Maps instructions to operand values and store–call alias (which store
/// result is consumed by a call). Separate from AliasValues (store-based
/// may-alias used by the typestate analyzer).
class ValueTypeAlias {
public:
  ValueTypeAlias();

  ValueTypeAlias(const ValueTypeAlias &);
  ValueTypeAlias &operator=(const ValueTypeAlias &);

  void setValues(llvm::Instruction *instruction, Operands operand_values);
  Operands getValues(llvm::Instruction *value);
  bool exists(llvm::Instruction *value);

  void setValues(fitx::Instruction instruction, Operands operand_values);
  Operands getValues(fitx::Instruction value);
  bool exists(fitx::Instruction value);

  Operands getAliasedValues(fitx::Instruction value);
  void setStoreAlias(llvm::StoreInst *store_inst, llvm::CallInst *call_inst);

  fitx::Instruction getAliasedStore(llvm::CallInst *call_inst);
  fitx::Instruction getAliasedStore(fitx::Instruction instruction);

  bool InstructionAliasExists(llvm::CallInst *call_inst);
  bool InstructionAliasExists(fitx::Instruction instruction);
  std::shared_ptr<std::map<fitx::Instruction, Operands>> AliasedValue() {
    return aliased_value_;
  };

private:
  std::shared_ptr<std::map<fitx::Instruction, Operands>> aliased_value_;
  std::map<fitx::Instruction, fitx::Instruction> store_alias_;
};
} // namespace fitx
