#include "Checker/FiTx/Core/SFG/Converter.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/Utils.h"

namespace fitx {

Converter &Converter::GetInstance() {
  // We use static local variable in order to achieve singleton here
  static Converter *converter = new Converter();
  return *converter;
}

struct Converter::ValueSignature
Converter::GetSignitureFromDefinition(llvm::Value *value) {
  bool finish_update = false;

  ValueSignature signature{value, fitx::Value::kNonArrayElement,
                           std::vector<fitx::Value::Fields>()};
  llvm::LoadInst *found_load_inst = nullptr;
  llvm::Value *start_value = value;

  auto UpdateFields =
      [&signature](std::vector<fitx::Value::Fields> new_fields) {
        return signature.fields.empty() ? new_fields : signature.fields;
      };

  while (auto *inst = llvm::dyn_cast<llvm::Instruction>(signature.value)) {
    switch (inst->getOpcode()) {
    case llvm::Instruction::Store:
      signature.value = llvm::cast<llvm::StoreInst>(inst)->getPointerOperand();
      break;
    case llvm::Instruction::Load: {
      auto *load_inst = llvm::cast<llvm::LoadInst>(inst);
      if (found_load_inst &&
          found_load_inst->getPointerOperand() == load_inst) {
        signature.fields = UpdateFields(std::vector<Value::Fields>(
            {Value::Fields(found_load_inst->getType())}));
      }
      signature.value = load_inst->getPointerOperand();
      found_load_inst = load_inst;
      break;
    }
    case llvm::Instruction::BitCast: {
      auto *bit_cast_inst = llvm::cast<llvm::BitCastInst>(inst);
      auto *source_type = bit_cast_inst->getSrcTy();
      auto *dest_type = bit_cast_inst->getDestTy();

      if (source_type->isPointerTy()) {
        auto count_pointer = [](llvm::Type *type) {
          int i = 0;
          while (type->isPointerTy()) {
            type = type->getPointerElementType();
            i++;
          }
          return i;
        };

        auto *element_source_type = getRootElementType(source_type);
        if ((count_pointer(source_type) + 1) == count_pointer(dest_type) &&
            element_source_type->isStructTy() &&
            element_source_type->getStructNumElements() > 0 &&
            getRootElementType(element_source_type->getStructElementType(0)) ==
                getRootElementType(dest_type)) {
          signature.fields = UpdateFields(std::vector<Value::Fields>(
              {Value::Fields(source_type->getPointerElementType(), 0)}));
        }

        if (found_load_inst &&
            (count_pointer(source_type) + 1) == count_pointer(dest_type) &&
            getRootElementType(dest_type)->isIntegerTy() &&
            count_pointer(found_load_inst->getType()) + 1 ==
                count_pointer(dest_type) &&
            !llvm::isa<llvm::GetElementPtrInst>(bit_cast_inst->getOperand(0))) {
          if (element_source_type->isStructTy())
            signature.fields = UpdateFields(std::vector<Value::Fields>(
                {Value::Fields(source_type->getPointerElementType(), 0)}));
          else
            signature.fields = UpdateFields(
                std::vector<Value::Fields>({Value::Fields(dest_type)}));
        }
        source_type = source_type->getPointerElementType();
      }

      if (source_type->isStructTy() &&
          source_type->getStructNumElements() > 0 && dest_type->isPointerTy()) {
        if (source_type->getStructElementType(0) ==
            dest_type->getPointerElementType()) {
          signature.fields = UpdateFields(
              std::vector<Value::Fields>({Value::Fields(source_type, 0)}));
        }
      } else if (source_type->isIntegerTy()) {
        if (dest_type->isPointerTy() &&
            dest_type->getPointerElementType()->isStructTy()) {
          signature.fields = UpdateFields(
              std::vector<Value::Fields>({Value::Fields(dest_type)}));
        }
      }
      if (getRootElementType(source_type) == getRootElementType(dest_type) &&
          source_type->isPointerTy()) {
        signature.fields = UpdateFields(
            std::vector<Value::Fields>({Value::Fields(source_type)}));
      }
      found_load_inst = nullptr;
      signature.value = inst->getOperand(0);
      break;
    }
    case llvm::Instruction::GetElementPtr: {
      auto *get_element_ptr_inst = llvm::cast<llvm::GetElementPtrInst>(inst);
      auto decode_types = decodeGetElementPtrInst(get_element_ptr_inst);
      signature.array_element_num =
          signature.array_element_num != fitx::Value::kNonArrayElement
              ? signature.array_element_num
              : arrayElementNum(get_element_ptr_inst);

      if (decode_types.size()) {
        signature.fields.insert(signature.fields.begin(), decode_types.begin(),
                                decode_types.end());
      }
      signature.value = get_element_ptr_inst->getPointerOperand();
      break;
    }
    case llvm::Instruction::Alloca: {
      signature.fields = UpdateFields(std::vector<Value::Fields>({Value::Fields(
          llvm::cast<llvm::AllocaInst>(inst)->getAllocatedType())}));
      for (llvm::User *U : inst->users()) {
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(U)) {
          if (llvm::isa<llvm::Argument>(store->getValueOperand())) {
            signature.value = store->getValueOperand();
          }
        }
      }
    }
    case llvm::Instruction::Call:
      /* break; */
    default:
      if (llvm::isa<llvm::CastInst>(inst)) {
        signature.value = inst->getOperand(0);
        break;
      }
      finish_update = true;
    }
    if (start_value == signature.value)
      break;
    if (finish_update)
      break;
  }

  if (auto *constant_expr =
          llvm::dyn_cast<llvm::ConstantExpr>(signature.value)) {
    if (auto *global_var = llvm::dyn_cast<llvm::GlobalVariable>(
            constant_expr->getOperand(0))) {
      signature.value = global_var;
    }
  }

  signature.fields = UpdateFields(
      std::vector<Value::Fields>({Value::Fields(value->getType())}));

  return signature;
}

std::shared_ptr<fitx::Value> Converter::Convert(llvm::Value *llvm_value) {
  auto signature = GetSignitureFromDefinition(llvm_value);
  if (llvm::isa<llvm::Instruction>(signature.value))
    return ConvertInstruction(signature);
  return ConvertValue(signature);
}

std::shared_ptr<fitx::Instruction>
Converter::ConvertInstruction(ValueSignature signature) {
  auto *llvm_instruction = llvm::cast<llvm::Instruction>(signature.value);

  if (auto managed_instruction =
          getManagedInst<fitx::Instruction>(signature))
    return managed_instruction;

  // If the value is instruction, we should create a corresponding value
  if (auto *inst = llvm::dyn_cast<llvm::CallInst>(signature.value)) {
    return fitx::CallInst::Create(inst, signature.fields,
                                       signature.array_element_num);
  }

  auto value = std::make_shared<fitx::Instruction>(
      llvm_instruction, signature.fields, signature.array_element_num);
  manageValue(signature.value, value);
  return value;
}

std::shared_ptr<fitx::Value>
Converter::ConvertValue(ValueSignature signature) {
  /* add to signiture list */
  if (auto managed_value = getManagedValue(signature))
    return managed_value;

  std::shared_ptr<fitx::Value> value;
  switch (signature.value->getValueID()) {
  case llvm::Value::ConstantIntVal:
    value = std::make_shared<fitx::ConstValue>(
        llvm::cast<llvm::ConstantInt>(signature.value));
    break;
  case llvm::Value::ConstantPointerNullVal:
    value = std::make_shared<fitx::NullValue>(
        llvm::cast<llvm::ConstantPointerNull>(signature.value));
    break;
  case llvm::Value::ArgumentVal:
    value = std::make_shared<fitx::Argument>(
        llvm::cast<llvm::Argument>(signature.value), signature.fields,
        signature.array_element_num);
    break;
  default:
    value = std::make_shared<fitx::Value>(
        signature.value, signature.fields, signature.array_element_num);
  }

  manageValue(signature.value, value);
  return value;
}

void Converter::manageValue(llvm::Value *value,
                            std::shared_ptr<fitx::Value> framework_value) {
  ManagedValues::GetInstance().addValue(framework_value);
  managed_values_[value].push_back(framework_value);
}

std::shared_ptr<fitx::Value>
Converter::getManagedValue(llvm::Value *value, long array_element_num,
                           std::vector<fitx::Value::Fields> fields) {
  return getManagedValue(ValueSignature{value, array_element_num, fields});
}

std::shared_ptr<fitx::Value>
Converter::getManagedValue(ValueSignature signature) {
  auto managed = managed_values_.find(signature.value);
  if (managed != managed_values_.end()) {
    auto &values = managed->second;
    auto found_value = std::find_if(
        values.begin(), values.end(),
        [&signature](std::shared_ptr<fitx::Value> value) {
          return &value->getLLVMValue_() == signature.value &&
                 value->ArrayElementNum() == signature.array_element_num &&
                 value->GetFields() == signature.fields;
        });
    if (found_value != values.end())
      return *found_value;
  }

  return std::shared_ptr<fitx::Value>();
}

} // namespace fitx
