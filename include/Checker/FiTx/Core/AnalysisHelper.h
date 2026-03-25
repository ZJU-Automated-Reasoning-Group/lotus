/// \file AnalysisHelper.h
/// \brief Helpers for decoding GEP/struct layout and predecessor checks.
/// Used when creating framework Values (field paths) and during CFG traversal.
#pragma once
#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Value.h"

// include STL
#include <vector>

namespace fitx {
std::vector<fitx::Value::Fields>
decodeGetElementPtrInst(llvm::GetElementPtrInst *get_element_ptr_inst);
long arrayElementNum(llvm::GetElementPtrInst *get_element_ptr_inst);

static std::vector<long> getValueIndices(llvm::GetElementPtrInst *inst);
static long getMemberIndiceFromByte(llvm::Instruction *instruction,
                                    llvm::StructType *STy, uint64_t byte);
static const llvm::StructLayout *getStructLayout(llvm::Instruction *instruction,
                                                 llvm::StructType *STy);

bool isInPredecessor(std::shared_ptr<fitx::BasicBlock> target,
                     std::shared_ptr<fitx::BasicBlock> block, int depth);
} // namespace fitx
