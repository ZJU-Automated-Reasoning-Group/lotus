#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"

namespace fitx {
class LoadInst : public Instruction {
 public:
  static std::shared_ptr<LoadInst> Create(
      llvm::LoadInst* load_inst,
      std::vector<Fields> field = std::vector<Fields>(),
      long array_element_num = Value::kNonArrayElement);

  LoadInst(llvm::LoadInst* load_inst);
  LoadInst(llvm::LoadInst* instruction, std::vector<Fields> fields,
           long array_element_num);

  std::shared_ptr<fitx::Value> LoadValue() {return load_value_; }
  llvm::Type* PointerType() {return pointer_type_; }

  void setValue(std::shared_ptr<fitx::Value>);

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static bool classof(const fitx::Instruction* I) {
    return I->Opcode() == llvm::Instruction::Load;
  }

  static bool classof(const fitx::Value* V) {
    return llvm::isa<fitx::Instruction>(V) &&
           classof(llvm::cast<fitx::Instruction>(V));
  }

 private:
  std::shared_ptr<fitx::Value> load_value_;
  llvm::Type* pointer_type_;
};
}  // namespace fitx
