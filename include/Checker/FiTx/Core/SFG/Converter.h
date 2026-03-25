#pragma once
#include <memory>
#include <vector>

#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Core/Value.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

namespace fitx {

class Converter {
 public:
  struct ValueSignature {
    llvm::Value* value;
    long array_element_num;
    std::vector<fitx::Value::Fields> fields;
  };

  // factory
  static Converter& GetInstance();

  struct ValueSignature GetSignitureFromDefinition(llvm::Value* value);

  std::shared_ptr<fitx::Value> Convert(llvm::Value* llvm_value);

  std::shared_ptr<fitx::Instruction> ConvertInstruction(
      ValueSignature signature);
  std::shared_ptr<fitx::Value> ConvertValue(ValueSignature signature);

  void manageValue(llvm::Value* value,
                   std::shared_ptr<fitx::Value> framework_value);

  template <class FrameworkClass, class LLVMClass>
  std::shared_ptr<FrameworkClass> createManagedInst(
      LLVMClass* llvm_inst, long array_element_num,
      std::vector<Value::Fields> fields,
      std::function<
          std::shared_ptr<FrameworkClass>(std::shared_ptr<FrameworkClass>)>
          post_process = std::function<std::shared_ptr<FrameworkClass>(
              std::shared_ptr<FrameworkClass>)>()) {
    if (fields.empty()) {
      fields.push_back(Value::Fields(llvm_inst->getType()));
    }
    if (auto managed = Converter::GetInstance().getManagedInst<FrameworkClass>(
            llvm_inst, array_element_num, fields))
      return managed;

    auto created =
        std::make_shared<FrameworkClass>(llvm_inst, fields, array_element_num);
    Converter::GetInstance().manageValue(llvm_inst, created);

    if (!post_process) return created;
    return post_process(created);
  }

  template <class FrameworkClass>
  std::shared_ptr<FrameworkClass> createManagedInst(
      std::shared_ptr<FrameworkClass> inst, long array_element_num,
      std::vector<Value::Fields> fields,
      std::function<
          std::shared_ptr<FrameworkClass>(std::shared_ptr<FrameworkClass>)>
          post_process = std::function<std::shared_ptr<FrameworkClass>(
              std::shared_ptr<FrameworkClass>)>()) {
    if (auto managed = Converter::GetInstance().getManagedInst<FrameworkClass>(
            inst->LLVMInstruction(), array_element_num, fields))
      return managed;

    auto created =
        std::make_shared<FrameworkClass>(inst, fields, array_element_num);
    Converter::GetInstance().manageValue(inst->LLVMInstruction(), created);

    if (!post_process) return created;
    return post_process(created);
  }

  template <class FrameworkClass>
  std::shared_ptr<FrameworkClass> getManagedInst(
      llvm::Instruction* inst, long array_element_num = Value::kNonArrayElement,
      std::vector<fitx::Value::Fields> fields =
          std::vector<fitx::Value::Fields>()) {
    auto managed =
        getManagedValue(ValueSignature{inst, array_element_num, fields});

    if (managed && fitx::shared_isa<FrameworkClass>(managed))
      return fitx::shared_dyn_cast<FrameworkClass>(managed);

    return std::shared_ptr<FrameworkClass>();
  }

  template <class FrameworkClass>
  std::shared_ptr<FrameworkClass> getManagedInst(ValueSignature signature) {
    auto managed = getManagedValue(signature);

    if (managed && fitx::shared_isa<FrameworkClass>(managed))
      return fitx::shared_dyn_cast<FrameworkClass>(managed);

    return std::shared_ptr<FrameworkClass>();
  }

  std::shared_ptr<fitx::Value> getManagedValue(
      llvm::Value* value, long array_element_num = Value::kNonArrayElement,
      std::vector<fitx::Value::Fields> fields =
          std::vector<fitx::Value::Fields>());
  std::shared_ptr<fitx::Value> getManagedValue(ValueSignature signature);

 private:
  Converter() = default;

  std::map<llvm::Value*, std::vector<std::shared_ptr<fitx::Value>>>
      managed_values_;
};
}  // namespace fitx
