#include "Checker/FiTx/Core/AnalysisHelper.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"

#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"

// include STL
#include <vector>

namespace fitx {
std::vector<fitx::Value::Fields>
decodeGetElementPtrInst(llvm::GetElementPtrInst *get_element_ptr_inst) {
  llvm::Type *Ty = get_element_ptr_inst->getPointerOperandType();
  /* llvm::Type *Ty = get_element_ptr_inst->getSourceElementType(); */
  std::vector<long> indice = getValueIndices(get_element_ptr_inst);
  std::vector<fitx::Value::Fields> decoded;

  for (long ind : indice) {
    long index = ind;
    if (Ty && index != Value::kNonFieldVariable) {
      if (Ty->isIntegerTy()) {
        if (Ty && getRootElementType(Ty)->isStructTy()) {
          index = getMemberIndiceFromByte(
              get_element_ptr_inst,
              llvm::cast<llvm::StructType>(getRootElementType(Ty)), index);
        }
      }
      decoded.push_back(fitx::Value::Fields(Ty, index));
      if (auto *StTy = llvm::dyn_cast<llvm::StructType>(getRootElementType(Ty)))
        Ty = StTy->getElementType(index);
      /* if (auto StTy = llvm::dyn_cast<llvm::StructType>(Ty)) */
      /*   Ty = StTy->getElementType(index); */
    } else {
      // decoded.push_back(std::pair<llvm::Type *, long>(NULL, ROOT_INDEX));
    }
  }

  return decoded;
}

long arrayElementNum(llvm::GetElementPtrInst *get_element_ptr_inst) {
  const unsigned num_ops = get_element_ptr_inst->getNumOperands();
  if (num_ops < 2)
    return fitx::Value::kArbitaryArrayElement;

  if (get_element_ptr_inst->getSourceElementType()->isArrayTy()) {
    // Use the last index (array element index); idx_end() is past-the-end.
    llvm::Value *last_idx_val = get_element_ptr_inst->getOperand(num_ops - 1);
    if (auto *const_int = llvm::dyn_cast<llvm::ConstantInt>(last_idx_val)) {
      if (const_int->getBitWidth() > 0)
        return const_int->getSExtValue();
    }
    return fitx::Value::kArbitaryArrayElement;
  }

  llvm::Value *first_idx_val = get_element_ptr_inst->getOperand(1);
  if (auto *const_int = llvm::dyn_cast<llvm::ConstantInt>(first_idx_val)) {
    if (const_int->getBitWidth() == 0)
      return fitx::Value::kArbitaryArrayElement;
    if (const_int->isZero())
      return fitx::Value::kNonArrayElement;
    return const_int->getSExtValue();
  }
  return fitx::Value::kArbitaryArrayElement;
}

std::vector<long> getValueIndices(llvm::GetElementPtrInst *inst) {
  std::vector<long> indices;

  llvm::Type *Ty = inst->getSourceElementType();
  auto *idx_itr = inst->idx_begin();
  if (llvm::isa<llvm::ConstantInt>(idx_itr->get())) {
    if (!Ty->isIntegerTy())
      idx_itr++;
  }

  for (; idx_itr != inst->idx_end(); idx_itr++) {
    long indice = Value::kNonFieldVariable;
    if (llvm::ConstantInt *cint =
            llvm::dyn_cast<llvm::ConstantInt>(idx_itr->get()))
      indice = cint->getSExtValue();
    indices.push_back(indice);
  }

  return indices;
}

long getMemberIndiceFromByte(llvm::Instruction *instruction,
                             llvm::StructType *STy, uint64_t byte) {
  const llvm::StructLayout *sl = getStructLayout(instruction, STy);
  if (sl != NULL)
    return sl->getElementContainingOffset(byte);
  return Value::kNonFieldVariable;
}

const llvm::StructLayout *getStructLayout(llvm::Instruction *instruction,
                                          llvm::StructType *STy) {
  auto *llvm_function = instruction->getFunction();
  if (!llvm_function)
    return nullptr;

  auto *llvm_module = llvm_function->getParent();
  if (!llvm_module)
    return nullptr;

  return llvm_module->getDataLayout().getStructLayout(STy);
}

bool isInPredecessor(std::shared_ptr<fitx::BasicBlock> target,
                     std::shared_ptr<fitx::BasicBlock> block, int depth) {
  if (depth < 0)
    return false;

  for (auto pred_ref : block->Predecessors()) {
    auto pred = pred_ref.lock();
    if (!pred)
      continue;

    if (pred == target || isInPredecessor(target, pred, depth - 1))
      return true;
  }

  return false;
}

} // namespace fitx
