/**
 * @file DebugInfoUtils.cpp
 * @brief Implementation of debug information utilities for the PDG system
 *
 * This file provides utility functions for accessing and processing LLVM debug
 * information (DIType, DIVariable, etc.) within the PDG system. These utilities
 * enable the PDG to leverage debug information for more precise analysis.
 *
 * Key functionality:
 * - Extraction of type information from variables and instructions
 * - Handling of complex types (structs, arrays, pointers)
 * - Support for field-sensitive analysis through type information
 * - Mapping between LLVM IR values and their debug information
 * - Access to variable source-level information (name, file, line)
 * - Support for C++ class hierarchy analysis
 *
 * The debug information utilities are particularly important for
 * field-sensitive analysis and for providing meaningful information in PDG
 * visualizations.
 */

#include "IR/PDG/Support/DebugInfoUtils.h"

using namespace llvm;

namespace {
static std::string getTypeNameOrEmpty(DIType *type) {
  if (!type)
    return "";
  return pdg::dbgutils::getSourceLevelTypeName(*type);
}
} // namespace

// ===== check types =====

bool pdg::dbgutils::isPointerType(DIType &dt) {
  return (dt.getTag() == dwarf::DW_TAG_pointer_type);
}

bool pdg::dbgutils::isReferenceType(DIType &dt) {
  return (dt.getTag() == dwarf::DW_TAG_reference_type || isPointerType(dt));
}

bool pdg::dbgutils::isStructType(DIType &dt) {
  return (dt.getTag() == dwarf::DW_TAG_structure_type || isClassType(dt));
}

bool pdg::dbgutils::isClassType(DIType &dt) {
  return (dt.getTag() == dwarf::DW_TAG_class_type);
}

bool pdg::dbgutils::isClassPointerType(DIType &dt) {
  if (isReferenceType(dt)) {
    DIType *lowest_di_type = getLowestDIType(dt);
    if (lowest_di_type == nullptr)
      return false;
    if (isClassType(*lowest_di_type))
      return true;
  }
  return false;
}

bool pdg::dbgutils::isUnionType(DIType &dt) {
  return (dt.getTag() == dwarf::DW_TAG_union_type);
}

bool pdg::dbgutils::isStructPointerType(DIType &dt) {
  if (isReferenceType(dt)) {
    DIType *lowest_di_type = getLowestDIType(dt);
    if (lowest_di_type == nullptr)
      return false;
    if (isStructType(*lowest_di_type))
      return true;
  }
  return false;
}

bool pdg::dbgutils::isFuncPointerType(DIType &dt) {
  DIType *di = stripMemberTag(dt);
  if (!di)
    return false;
  if (di->getTag() == dwarf::DW_TAG_subroutine_type ||
      isa<DISubroutineType>(di) || isa<DISubprogram>(di))
    return true;
  auto *lowest_di_type = getLowestDIType(*di);
  if (lowest_di_type != nullptr)
    return (lowest_di_type->getTag() == dwarf::DW_TAG_subroutine_type) ||
           isa<DISubroutineType>(lowest_di_type) ||
           isa<DISubprogram>(lowest_di_type);
  return false;
}

bool pdg::dbgutils::isProjectableType(DIType &dt) {
  return (isStructType(dt) || isUnionType(dt));
}

bool pdg::dbgutils::hasSameDIName(DIType &d1, DIType &d2) {
  std::string d1_name = dbgutils::getSourceLevelTypeName(d1);
  std::string d2_name = dbgutils::getSourceLevelTypeName(d2);
  return (d1_name == d2_name);
}

// ===== derived types related operations =====
DIType *pdg::dbgutils::getBaseDIType(DIType &dt) {
  if (DIDerivedType *derived_ty = dyn_cast<DIDerivedType>(&dt))
    return derived_ty->getBaseType();
  return nullptr;
}

DIType *pdg::dbgutils::getLowestDIType(DIType &dt) {
  DIType *current_dt = &dt;
  while (DIDerivedType *derived_dt = dyn_cast<DIDerivedType>(current_dt)) {
    current_dt = derived_dt->getBaseType();
    if (!current_dt) // could happen for a pointer to void pointer etc
      break;
  }
  return current_dt;
}

DIType *pdg::dbgutils::stripAttributes(DIType &dt) {
  DIType *current_dt = &dt;
  if (!current_dt)
    return nullptr;
  auto type_tag = dt.getTag();
  while (type_tag == dwarf::DW_TAG_typedef ||
         type_tag == dwarf::DW_TAG_const_type ||
         type_tag == dwarf::DW_TAG_volatile_type ||
         type_tag == dwarf::DW_TAG_restrict_type ||
         type_tag == dwarf::DW_TAG_atomic_type) {
    auto *didt = dyn_cast<DIDerivedType>(current_dt);
    if (!didt)
      return current_dt;
    DIType *baseTy = didt->getBaseType();
    if (baseTy == nullptr)
      return nullptr;
    current_dt = baseTy;
    type_tag = current_dt->getTag();
  }
  return current_dt;
}

DIType *pdg::dbgutils::stripMemberTag(DIType &dt) {
  auto type_tag = dt.getTag();
  if (type_tag == dwarf::DW_TAG_member)
    return getBaseDIType(dt);
  return &dt;
}

// ===== get the source level naming information for variable or types =====
std::string pdg::dbgutils::getSourceLevelVariableName(DINode &di_node) {
  if (DILocalVariable *di_var = dyn_cast<DILocalVariable>(&di_node)) {
    return di_var->getName().str();
  }

  // get field name
  if (DIType *dt = dyn_cast<DIType>(&di_node)) {
    auto type_tag = dt->getTag();
    switch (type_tag) {
    case dwarf::DW_TAG_member: {
      return dt->getName().str();
    }
    case dwarf::DW_TAG_structure_type:
      return dt->getName().str();
    case dwarf::DW_TAG_typedef:
      return dt->getName().str();
    default:
      return dt->getName().str();
    }
  }
  return "";
}

std::string pdg::dbgutils::getSourceLevelTypeName(DIType &dt) {
  auto type_tag = dt.getTag();
  if (!type_tag)
    return "";
  switch (type_tag) {
  case dwarf::DW_TAG_pointer_type: {
    auto *base_type = getBaseDIType(dt);
    std::string base_type_name = getTypeNameOrEmpty(base_type);
    if (base_type_name.empty())
      base_type_name = "void";
    return base_type_name + "*";
  }
  case dwarf::DW_TAG_class_type: {
    return "class " + dt.getName().str();
  }
  case dwarf::DW_TAG_member: {
    auto *base_type = getBaseDIType(dt);
    if (!base_type)
      return "";
    std::string base_type_name = getSourceLevelTypeName(*base_type);
    if (base_type_name == "struct")
      base_type_name = "struct " + dt.getName().str();
    return base_type_name;
  }
  case dwarf::DW_TAG_structure_type: {
    if (dt.getName().empty())
      return "struct";
    return "struct " + dt.getName().str();
  }
  case dwarf::DW_TAG_array_type: {
    return "array";
  }
  case dwarf::DW_TAG_const_type: {
    auto *base_type = getBaseDIType(dt);
    std::string base_type_name = getTypeNameOrEmpty(base_type);
    if (base_type_name.empty())
      return "const";
    return "const " + base_type_name;
  }
  case dwarf::DW_TAG_volatile_type: {
    auto *base_type = getBaseDIType(dt);
    std::string base_type_name = getTypeNameOrEmpty(base_type);
    if (base_type_name.empty())
      return "volatile";
    return "volatile " + base_type_name;
  }
  default: {
    return dt.getName().str();
  }
  }
  return "";
}

// compute di type for value
DIType *pdg::dbgutils::getGlobalVarDIType(GlobalVariable &gv) {
  SmallVector<DIGlobalVariableExpression *, 5> GVs;
  gv.getDebugInfo(GVs);
  if (GVs.size() == 0)
    return nullptr;
  for (auto *GV : GVs) {
    if (!GV)
      continue;
    DIGlobalVariable *digv = GV->getVariable();
    if (!digv)
      continue;
    return digv->getType();
  }
  return nullptr;
}

DIType *pdg::dbgutils::getFuncRetDIType(Function &F) {
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  F.getAllMetadata(MDs);
  for (auto &MD : MDs) {
    MDNode *N = MD.second;
    if (DISubprogram *subprogram = dyn_cast<DISubprogram>(N)) {
      auto *sub_routine = subprogram->getType();
      if (!sub_routine)
        return nullptr;
      const auto &type_ref = sub_routine->getTypeArray();
      if (type_ref.size() == 0)
        return nullptr;
      return type_ref[0];
    }
  }
  return nullptr;
}

std::set<DIType *> pdg::dbgutils::computeContainedStructTypes(DIType &dt) {
  std::set<DIType *> contained_struct_di_types;
  if (!isStructType(dt))
    return contained_struct_di_types;
  std::queue<DIType *> type_queue;
  type_queue.push(&dt);
  int current_tree_height = 0;
  int max_tree_height = 5;
  while (current_tree_height < max_tree_height) {
    current_tree_height++;
    int queue_size = type_queue.size();
    while (queue_size > 0) {
      queue_size--;
      DIType *current_di_type = type_queue.front();
      type_queue.pop();
      if (!isStructType(*current_di_type))
        continue;
      if (contained_struct_di_types.find(current_di_type) !=
          contained_struct_di_types.end())
        continue;
      if (getSourceLevelTypeName(*current_di_type).compare("struct") ==
          0) // ignore anonymous struct
        continue;
      contained_struct_di_types.insert(current_di_type);
      auto *composite_type = dyn_cast<DICompositeType>(current_di_type);
      if (!composite_type)
        continue;
      auto di_node_arr = composite_type->getElements();
      for (unsigned i = 0; i < di_node_arr.size(); i++) {
        DIType *field_di_type = dyn_cast<DIType>(di_node_arr[i]);
        if (!field_di_type)
          continue;
        DIType *field_lowest_di_type = getLowestDIType(*field_di_type);
        if (!field_lowest_di_type)
          continue;
        if (isStructType(*field_lowest_di_type))
          type_queue.push(field_lowest_di_type);
      }
    }
  }
  return contained_struct_di_types;
}
