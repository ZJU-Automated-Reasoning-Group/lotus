#pragma once
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

namespace fitx {
class StoreInst : public Instruction {
 public:
  static std::shared_ptr<StoreInst> Create(
      llvm::StoreInst* load_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  StoreInst(llvm::StoreInst* store_inst, std::vector<Fields> fields,
           long array_element_num);
  StoreInst(llvm::StoreInst* store_inst);

  std::shared_ptr<fitx::Value> ValueOperand() const {return value_; };
  std::shared_ptr<fitx::Value> PointerOperand() const {return pointer_; };

  void setValue(std::shared_ptr<fitx::Value> value);
  void setPointer(std::shared_ptr<fitx::Value> pointer);

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static bool classof(const fitx::Instruction* I) {
    return I->Opcode() == llvm::Instruction::Store;
  }

  static bool classof(const fitx::Value* V) {
    return llvm::isa<fitx::Instruction>(V) &&
           classof(llvm::cast<fitx::Instruction>(V));
  }

 private:
  std::shared_ptr<fitx::Value> value_;
  std::shared_ptr<fitx::Value> pointer_;
};

}  // namespace fitx
