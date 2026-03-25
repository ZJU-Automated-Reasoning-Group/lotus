#include "Checker/FiTx/Core/Instructions/LoadInstruction.h"

#include "llvm/IR/InstrTypes.h"

#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Value.h"

namespace fitx {
std::shared_ptr<LoadInst> LoadInst::Create(llvm::LoadInst *load_inst,
                                           std::vector<Value::Fields> fields,
                                           long array_element_num) {
  auto created =
      Converter::GetInstance().createManagedInst<fitx::LoadInst>(
          load_inst, array_element_num, fields,
          [load_inst](std::shared_ptr<LoadInst> created) {
            auto operand =
                Value::CreateFromDefinition(load_inst->getPointerOperand());
            created->setValue(operand);
            return created;
          });

  return created;
}

void LoadInst::setValue(std::shared_ptr<fitx::Value> value) {
  load_value_ = value;
}

LoadInst::LoadInst(llvm::LoadInst *load_inst)
    : Instruction(load_inst),
      load_value_(fitx::Value::CreateFromDefinition(
          load_inst->getPointerOperand())) {}

LoadInst::LoadInst(llvm::LoadInst *instruction, std::vector<Fields> fields,
                   long array_element_num)
    : Instruction(instruction, fields, array_element_num),
      pointer_type_(instruction->getType()) {}
} // namespace fitx
