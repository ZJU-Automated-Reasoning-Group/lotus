#include "llvm/IR/Value.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

// include STL
#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <vector>

namespace fitx {
Value::Value(llvm::Value *value, std::vector<Fields> fields,
             long array_element_num)
    : value_(value), fields_(fields), array_element_num_(array_element_num),
      is_global_var_(llvm::isa<llvm::GlobalValue>(value)),
      value_type_(value->getValueID()) {}

Value::Value(std::shared_ptr<fitx::Value> value,
             std::vector<Fields> fields, long array_element_num)
    : value_(value->value_), fields_(fields),
      array_element_num_(array_element_num),
      is_global_var_(value->isGlobalVar()),
      is_return_value_(value->is_return_value_),
      value_type_(value->getValueID()) {}

Value::Value(const Value &value)
    : value_(value.value_), fields_(value.fields_),
      array_element_num_(value.array_element_num_),
      is_global_var_(value.is_global_var_), value_type_(value.getValueID()) {}

Value::Value(std::shared_ptr<Value> value) {
  value_ = value->value_;
  array_element_num_ = value->array_element_num_;
  fields_ = value->fields_;
  is_global_var_ = value->isGlobalVar();
  is_return_value_ = value->is_return_value_;
  value_type_ = value->getValueID();
}

std::shared_ptr<Value> Value::CreateFromDefinition(llvm::Value *value) {
  return fitx::Converter::GetInstance().Convert(value);
}

std::shared_ptr<Value> Value ::CreateAppend(std::shared_ptr<Value> src,
                                            std::shared_ptr<Value> target) {
  auto source_back = src->GetFields().end() - 1;
  auto target_front = target->GetFields().begin();

  if (!src->GetFields().empty() && !target->GetFields().empty()) {
    llvm::Type *type = nullptr;
    if (source_back->FieldType() == target_front->ElementType() &&
        !target_front->FieldType()) {
      target_front++;
    }
    while (source_back >= src->GetFields().begin() &&
           target_front < target->GetFields().end() &&
           source_back->ElementType() == target_front->ElementType() &&
           source_back->FieldType() != target_front->type) {
      if (type && type->isPointerTy() &&
          type->getPointerElementType() == source_back->type &&
          type->getPointerElementType() == target_front->type) {
        target_front++;
      }
      type = (source_back--)->type;
    }
  }

  auto new_fields =
      std::vector<Fields>(src->GetFields().begin(), source_back + 1);
  new_fields.insert(new_fields.end(), target_front, target->GetFields().end());

  int array_element_num =
      std::max(target->ArrayElementNum(), src->ArrayElementNum());

  if (auto managed = Converter::GetInstance().getManagedValue(
          &src->getLLVMValue_(), array_element_num, new_fields)) {
    return managed;
  }

  if (auto call_inst = shared_dyn_cast<fitx::CallInst>(src))
    return fitx::CallInst::Create(call_inst, new_fields,
                                       array_element_num);

  if (auto inst = shared_dyn_cast<fitx::Instruction>(src))
    return fitx::Instruction::Create(inst, new_fields, array_element_num);

  std::shared_ptr<fitx::Value> value;
  switch (src->getValueID()) {
  case llvm::Value::ConstantIntVal:
    value = std::make_shared<fitx::ConstValue>(
        fitx::shared_dyn_cast<fitx::ConstValue>(src));
    break;
  case llvm::Value::ConstantPointerNullVal:
    value = std::make_shared<fitx::NullValue>(
        fitx::shared_dyn_cast<fitx::NullValue>(src));
    break;
  case llvm::Value::ArgumentVal:
    value = std::make_shared<fitx::Argument>(
        fitx::shared_dyn_cast<fitx::Argument>(src), new_fields,
        array_element_num);
    break;
  default:
    value =
        std::make_shared<fitx::Value>(src, new_fields, array_element_num);
  }

  Converter::GetInstance().manageValue(&src->getLLVMValue_(), value);
  return value;
}

bool Value::operator<(const Value &V) const {
  if (value_ != V.value_)
    return value_ < V.value_;

  if (array_element_num_ != V.array_element_num_)
    return array_element_num_ < V.array_element_num_;

  if (fields_.size() != V.fields_.size())
    return fields_.size() < V.fields_.size();

  if (value_type_ != V.value_type_)
    return value_type_ < V.value_type_;

  for (int i = 0; i < fields_.size(); i++) {
    if (fields_[i].type != V.fields_[i].type)
      return fields_[i].type < V.fields_[i].type;

    if (fields_[i].field != V.fields_[i].field)
      return fields_[i].field < V.fields_[i].field;
  }

  return false;
}

bool Value::operator==(const Value &V) const {
  if (value_ != V.value_ || fields_.size() != V.fields_.size() ||
      array_element_num_ != V.array_element_num_ ||
      value_type_ != V.value_type_)
    return false;

  for (int i = 0; i < fields_.size(); i++) {
    if (fields_[i].type != V.fields_[i].type ||
        fields_[i].field != V.fields_[i].field)
      return false;
  }
  return true;
}

bool Value::operator==(const llvm::Value *V) const { return value_ == V; }

llvm::Value &Value::getLLVMValue_() const { return *value_; }

llvm::Type &Value::getLLVMType_() const {
  const Fields &field = fields_.back();
  if (field.field >= 0 && field.type->isStructTy())
    return *field.type->getStructElementType(field.field);
  return *field.type;
}

long Value::Field() const { return fields_.back().field; }

const std::vector<Value::Fields> &Value::GetFields() const { return fields_; };

bool Value::isArgument() { return llvm::isa<llvm::Argument>(value_); }

bool Value::isGlobalVar() { return is_global_var_; }

bool Value::isReturnValue() { return is_return_value_; }
void Value::setReturnValue(bool is_return_value) {
  is_return_value_ = is_return_value;
}

long Value::ArrayElementNum() { return array_element_num_; }

ConstValue::ConstValue(llvm::ConstantInt *value)
    : Value(value), const_value_(value->getSExtValue()) {}

ConstValue::ConstValue(int64_t const_value)
    : const_value_(const_value), Value(llvm::Value::ConstantIntVal) {}

ConstValue::ConstValue(std::shared_ptr<ConstValue> value)
    : Value(value), const_value_(value->getConstValue()) {}

NullValue::NullValue(llvm::ConstantPointerNull *value) : Value(value) {}
NullValue::NullValue(std::shared_ptr<fitx::NullValue> value)
    : Value(value) {}

Argument::Argument(llvm::Argument *argument, std::vector<Value::Fields> fields,
                   long array_element_num)
    : Value(argument, fields, array_element_num),
      arg_num_(argument->getArgNo()) {}

Argument::Argument(std::shared_ptr<Argument> argument,
                   std::vector<Value::Fields> fields, long array_element_num)
    : Value(&argument->getLLVMValue_(), fields, array_element_num),
      arg_num_(argument->ArgNum()) {}

/* Value Collection Class*/
ValueCollection ValueCollection::createFromIntersection(
    const std::vector<ValueCollection> &collections) {
  if (collections.empty())
    return ValueCollection();

  ValueCollection result = *collections.begin();
  for (auto collection = collections.begin() + 1;
       collection != collections.end(); collection++) {
    ValueCollection tmp;
    set_intersection(result.Values().begin(), result.Values().end(),
                     collection->values_.begin(), collection->values_.end(),
                     inserter(tmp.values_, tmp.values_.end()));
    result = tmp;
  }
  return result;
}

ValueCollection ValueCollection::createFromUnion(
    const std::vector<ValueCollection> &collections) {
  if (collections.empty())
    return ValueCollection();

  ValueCollection result = *collections.begin();
  for (auto collection = collections.begin() + 1;
       collection != collections.end(); collection++) {
    ValueCollection tmp;
    set_union(result.Values().begin(), result.Values().end(),
              collection->values_.begin(), collection->values_.end(),
              inserter(tmp.values_, tmp.values_.end()));
    result = tmp;
  }
  return result;
}

bool ValueCollection::add(std::shared_ptr<Value> value) {
  return values_.insert(value).second;
}

void ValueCollection::add(const ValueCollection &value_collection) {
  auto &values = value_collection.Values();
  std::merge(values_.begin(), values_.end(), values.begin(), values.end(),
             std::inserter(values_, values_.end()));
}

void ValueCollection::remove(std::shared_ptr<Value> value) {
  values_.erase(value);
}

void ValueCollection::clear() { values_.clear(); }

bool ValueCollection::exists(fitx::Value value) {
  auto found_value =
      std::find_if(values_.begin(), values_.end(),
                   [&value](std::shared_ptr<Value> compared_value) {
                     return *compared_value == value;
                   });
  return found_value != values_.end();
}

/* std::shared_ptr<Value> ValueCollection::get(llvm::Value* value) { */
/*   auto framework_value = fitx::Value::CreateFromDefinition(value); */

/*   auto found_value = */
/*       std::find_if(values_.begin(), values_.end(), */
/*                    [framework_value](std::shared_ptr<Value> compared_value) {
 */
/*                      return *compared_value == *framework_value; */
/*                    }); */
/*   if (found_value != values_.end()) */
/*     return *found_value; */

/*   add(framework_value); */
/*   return framework_value; */
/* } */

bool ValueCollection::exists(std::shared_ptr<Value> value) {
  return values_.find(value) != values_.end();
}

// Same base LLVM value and compatible field path (same or more specific);
// used to apply store/branch transitions to all SSA/field variants (paper §3).
std::set<std::shared_ptr<Value>>
ValueCollection::getRelatedValues(std::shared_ptr<Value> value) const {
  std::set<std::shared_ptr<Value>> related_values;
  std::copy_if(
      values_.begin(), values_.end(),
      inserter(related_values, related_values.end()),
      [&value](std::shared_ptr<fitx::Value> new_value) {
        /* auto value_fields = value->GetFields(); */
        /* const std::vector<fitx::Value::Fields> sub_value_fields( */
        /*     value_fields.begin(), */
        /*     value_fields.begin() + */
        /*         std::min(value_fields.size(),
         * new_value->GetFields().size())); */

        return *new_value == &value->getLLVMValue_() &&
               new_value->GetFields().size() >= value->GetFields().size() &&
               (value->GetFields().empty() ||
                value->GetFields().back().field == Value::kNonFieldVariable ||
                value->GetFields().back().field ==
                    new_value->GetFields()[value->GetFields().size() - 1]
                        .field);
      });
  return related_values;
}

std::set<std::shared_ptr<Value>>
ValueCollection::getParentValues(std::shared_ptr<Value> value) const {
  std::set<std::shared_ptr<Value>> related_values;
  std::copy_if(values_.begin(), values_.end(),
               inserter(related_values, related_values.end()),
               [&value](std::shared_ptr<fitx::Value> new_value) {
                 return *new_value == &value->getLLVMValue_() &&
                        new_value->GetFields().size() <=
                            value->GetFields().size();
               });
  return related_values;
}

AliasValues::AliasValues() : alias_size_(0) {}

// Record may-alias: both directions so getAliasInfo works for either value.
void AliasValues::addAlias(std::shared_ptr<fitx::Value> src,
                           std::shared_ptr<fitx::Value> target) {
  if (alias_info_[src].add(target)) {
    updated_values_.push_back(src);
    alias_size_++;
  }

  if (alias_info_[target].add(src)) {
    updated_values_.push_back(target);
    alias_size_++;
  }
}

void AliasValues::addAlias(AliasValues &collection) {
  if (alias_size_ >= collection.Size())
    return;

  const std::vector<std::shared_ptr<fitx::Value>> &updated =
      collection.UpdatedValues();

  if (!updated.empty()) {
    for (auto value : updated) {
      // We know that getAliasInfo will return a valid pointer since it is added
      alias_info_[value].add(*collection.getAliasInfo(value));
      alias_size_ += alias_info_[value].size();
    }
    return;
  }

  for (auto alias_info : collection.AliasInfo()) {
    alias_info_[alias_info.first].add(alias_info.second);
    alias_size_ += alias_info_[alias_info.first].size();
  }
}

const ValueCollection *
AliasValues::getAliasInfo(std::shared_ptr<fitx::Value> value) {
  if (alias_info_.find(value) != alias_info_.end())
    return &alias_info_[value];
  return nullptr;
}

// Bug fix: return by reference (was returning by value, so addValue() calls
// on the returned copy were silently discarded).
ManagedValues &ManagedValues::GetInstance() {
  static ManagedValues *managed_values = new ManagedValues();
  return *managed_values;
}

ManagedValues::ManagedValues() { managed_values_.reserve(kReserveSize); }

std::shared_ptr<fitx::Value> ManagedValues::getValueFromID(size_t id) {
  if (id > Size())
    return nullptr;
  return managed_values_[id];
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              const fitx::Value &value) {
  ostream << "[fitx::Value] ";
  ostream << "ValueType: " << value.value_type_ << " ";
  ostream << "Array Element: " << value.array_element_num_ << " ";
  ostream << "(";
  for (auto field : value.fields_) {
    ostream << "{Type: ";
    if (field.type->isStructTy())
      ostream << field.type->getStructName();
    else
      ostream << *field.type;
    ostream << " Field: " << field.field << "}, ";
  }
  ostream << ")";

  return ostream;
}

/* std::vector<std::shared_ptr<fitx::Value>> Value::managed_values_; */

}; // namespace fitx
