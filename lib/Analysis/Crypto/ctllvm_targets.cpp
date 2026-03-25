/*
 * Taint-source and declassification selection for the ctllvm pass.
 */

#include "CTInternal.h"

using namespace llvm;

namespace ctllvm {
namespace detail {
namespace {

TargetValueInfo makeTargetValue(StringRef function_name, StringRef value_name,
                                StringRef value_type = "0",
                                StringRef field_name = "0",
                                int line_number = -1) {
  TargetValueInfo info;
  info.function_name = function_name.str();
  info.value_name = value_name.str();
  info.value_type = value_type.str();
  info.field_name = field_name.str();
  info.line_number = line_number;
  return info;
}

void appendDefaultTargetValues(std::vector<TargetValueInfo> &target_values) {
  target_values.push_back(
      makeTargetValue("mpi_powm", "exponent", "gcry_mpi", "d"));
  target_values.push_back(makeTargetValue("AES_ige_encrypt", "in"));
  target_values.push_back(makeTargetValue("ec_GF2m_montgomery_point_multiply",
                                          "scalar", "bignum_st", "d"));
  target_values.push_back(makeTargetValue("ec_wNAF_mul", "wNAF"));
}

} // namespace

bool CryptoAnalysisImpl::updateTargetValues(
    std::vector<TargetValueInfo> &target_values,
    std::vector<TargetValueInfo> &declassified_values) {
  appendDefaultTargetValues(target_values);
  return !target_values.empty() || !declassified_values.empty();
}

int CryptoAnalysisImpl::getFieldIndex(StructType *StructTy, StringRef FieldName,
                                      const Module &M) {
  DebugInfoFinder Finder;
  Finder.processModule(M);
  StringRef structure_name = StructTy->getName().drop_front(7);
  for (auto *Type : Finder.types()) {
    if (auto *Composite = dyn_cast<DICompositeType>(Type)) {
      if (Composite->getName() != structure_name) {
        continue;
      }
      unsigned index = 0;
      for (const auto *Element : Composite->getElements()) {
        if (auto *Member = dyn_cast<DIDerivedType>(Element)) {
          if (FieldName == Member->getName()) {
            return index;
          }
          index += 1;
        }
      }
    }
  }
  return -1;
}

bool CryptoAnalysisImpl::updateTaintList(
    Module &M, Function &F, Instruction &I, bool declassify_flag,
    SetVector<Value *> &tainted_values, ArrayRef<TargetValueInfo> entries) {
  bool handled_structure = false;
  bool handled_variable = false;

  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    if (auto *StructTy = dyn_cast<StructType>(GEP->getSourceElementType())) {
      for (const auto &target_value : entries) {
        if (target_value.function_name != F.getName().str()) {
          continue;
        }
        if (target_value.field_name == "0") {
          continue;
        }
        if (target_value.value_type != StructTy->getName().drop_front(7).str()) {
          continue;
        }
        if (auto *FieldIndex = dyn_cast<ConstantInt>(GEP->getOperand(2))) {
          if (FieldIndex->getZExtValue() ==
              static_cast<uint64_t>(
                  getFieldIndex(StructTy, target_value.field_name, M))) {
            Value *source_value = GEP->getPointerOperand();
            StringRef tmp_name = getDebugName(source_value, "", F);
            if (tmp_name != target_value.value_name) {
              continue;
            }
            if (declassify_flag &&
                getDebugLine(source_value, "", F) != target_value.line_number) {
              continue;
            }
            if (options_.debug) {
              errs() << "[FOUND.Structure] " << tmp_name << "\n";
            }
            tainted_values.insert(GEP);
            handled_structure = true;
          }
        }
      }
    }
  }

  if (handled_structure) {
    return true;
  }

  DILocalVariable *LocalVar = nullptr;
  Value *Arg = nullptr;

#if LLVM_VERSION_MAJOR >= 19
  for (DbgRecord &DR : I.getDbgRecordRange()) {
    if (auto *Dbg = dyn_cast<DbgVariableRecord>(&DR)) {
      LocalVar = Dbg->getVariable();
      Arg = Dbg->getValue();
    }
  }
#endif

  if (!LocalVar) {
    if (auto *DbgDeclare = dyn_cast<DbgDeclareInst>(&I)) {
      LocalVar = DbgDeclare->getVariable();
      Arg = DbgDeclare->getAddress();
    } else if (auto *DbgValue = dyn_cast<DbgValueInst>(&I)) {
      LocalVar = DbgValue->getVariable();
      Arg = DbgValue->getValue();
    }
  }

  if (!LocalVar) {
    return false;
  }

  for (const auto &target_value : entries) {
    if (target_value.function_name != F.getName().str()) {
      continue;
    }
    if (target_value.field_name != "0") {
      continue;
    }
    if (target_value.value_name != LocalVar->getName().str()) {
      continue;
    }
    if (declassify_flag &&
        getDebugLine(Arg, "", F) != target_value.line_number) {
      continue;
    }
    if (options_.debug) {
      errs() << "[FOUND.Variable] " << LocalVar->getName() << "\n";
    }
    tainted_values.insert(Arg);
    handled_variable = true;
  }

  return handled_variable;
}

} // namespace detail
} // namespace ctllvm
