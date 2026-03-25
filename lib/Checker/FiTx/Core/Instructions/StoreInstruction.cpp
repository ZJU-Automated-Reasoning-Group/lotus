#include "Checker/FiTx/Core/Instructions/StoreInstruction.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Value.h"

namespace fitx {
std::shared_ptr<StoreInst> StoreInst::Create(llvm::StoreInst *store_inst,
                                             std::vector<Value::Fields> fields,
                                             long array_element_num) {
  auto created =
      Converter::GetInstance().createManagedInst<fitx::StoreInst>(
          store_inst, array_element_num, fields,
          [store_inst](std::shared_ptr<StoreInst> created) {
            auto value_operand =
                Value::CreateFromDefinition(store_inst->getValueOperand());
            value_operand->addUser(created);
            created->setValue(value_operand);

            auto pointer_operand =
                Value::CreateFromDefinition(store_inst->getPointerOperand());
            pointer_operand->addUser(created);
            created->setPointer(pointer_operand);

            return created;
          });
  return created;
}

StoreInst::StoreInst(llvm::StoreInst *store_inst, std::vector<Fields> fields,
                     long array_element_num)
    : Instruction(store_inst, fields, array_element_num) {}

StoreInst::StoreInst(llvm::StoreInst *store_inst) : Instruction(store_inst) {
  value_ = Value::CreateFromDefinition(store_inst->getValueOperand());
  pointer_ = Value::CreateFromDefinition(store_inst->getPointerOperand());
}

void StoreInst::setValue(std::shared_ptr<fitx::Value> value) {
  value_ = value;
}

void StoreInst::setPointer(std::shared_ptr<fitx::Value> pointer) {
  pointer_ = pointer;
}
} // namespace fitx
