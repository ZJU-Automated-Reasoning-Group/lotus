#pragma once
#include <vector>

#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

namespace fitx {
class CallInst : public Instruction {
 public:
  static std::shared_ptr<CallInst> Create(
      llvm::CallInst* call_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  static std::shared_ptr<CallInst> Create(
      std::shared_ptr<CallInst> call_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  CallInst() = delete;
  CallInst(llvm::CallInst* call_inst);
  CallInst(llvm::CallInst* instruction, std::vector<Fields> fields,
           long array_element_num);

  CallInst(std::shared_ptr<CallInst> instruction, std::vector<Fields> fields,
           long array_element_num);

  CallInst& operator=(const CallInst& inst) {
    return_value_ = inst.return_value_;
    arguments_ = inst.arguments_;
    called_function_ = inst.called_function_;
    return *this;
  }

  void setCalledFunction(std::shared_ptr<fitx::Function> called_function) {
    called_function_ = called_function;
  }

  std::shared_ptr<fitx::Function> CalledFunction() {
    return called_function_;
  };

  const std::vector<std::shared_ptr<fitx::Value>>& Arguments() {
    return arguments_;
  }

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static bool classof(const fitx::Instruction* I) {
    return I->Opcode() == llvm::Instruction::Call;
  }

  static bool classof(const fitx::Value* V) {
    return llvm::isa<fitx::Instruction>(V) &&
           classof(llvm::cast<fitx::Instruction>(V));
  }

  void setArguments(std::vector<std::shared_ptr<fitx::Value>> arguments);

 private:
  std::shared_ptr<fitx::Value> return_value_;
  std::vector<std::shared_ptr<fitx::Value>> arguments_;

  std::shared_ptr<fitx::Function> called_function_;
};

}  // namespace fitx
