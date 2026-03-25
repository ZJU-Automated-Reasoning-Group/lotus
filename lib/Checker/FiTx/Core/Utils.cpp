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
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

// include STL
#include "llvm/Support/CommandLine.h"

#include "Checker/FiTx/Core/Instruction.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

static llvm::cl::opt<bool> Debug("fitx-debug",
                                 llvm::cl::desc("Print FiTx debug warnings"));

namespace fitx {
void generateWarning(llvm::Instruction *Inst, std::string warn) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << warn << "\n";
}

void generateWarning(llvm::Instruction *Inst, llvm::Value *val) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << *val << "\n";
}

void generateWarning(llvm::Instruction *Inst, llvm::Type *type) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << *type << "\n";
}

void generateWarning(std::string warning) {
  if (!Debug)
    return;
  llvm::errs() << warning << "\n";
}

void generateError(llvm::Instruction *Inst, std::string warn) {
  llvm::errs() << "[ERROR] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << warn << "\n";
}

std::string getDebugInfo(llvm::Instruction *inst) {
  if (const llvm::DebugLoc &Loc = inst->getDebugLoc()) {
    unsigned line = Loc.getLine();
    unsigned col = Loc.getCol();
    return std::string(Loc->getFilename()) + ":" + std::to_string(line) + ":" +
           std::to_string(col) + ": ";
  }
  return std::string();
}

std::string getFileName(llvm::Instruction *instruction) {
  if (const llvm::DebugLoc &Loc = instruction->getDebugLoc())
    return std::string(Loc->getFilename());
  return std::string();
}

int getLine(llvm::Instruction *instruction) {
  if (const llvm::DebugLoc &Loc = instruction->getDebugLoc())
    return Loc.getLine();
  return 0;
}

bool findFunctionName(const std::string &function_name,
                      const std::string &target_function) {
  return function_name.find(target_function) != std::string::npos;
}

llvm::Type *getRootElementType(llvm::Type *type) {
  llvm::Type *tmp_type = type;
  while (tmp_type->isPointerTy())
    tmp_type = tmp_type->getPointerElementType();
  return tmp_type;
}

// Framework Instruction Version
void generateWarning(fitx::Instruction *Inst, std::string warn) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << warn << "\n";
}

void generateWarning(fitx::Instruction *Inst, fitx::Value *val) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << *val << "\n";
}

void generateWarning(fitx::Instruction *Inst, llvm::Type *type) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << getDebugInfo(Inst);
  llvm::errs() << *type << "\n";
}

void generateError(llvm::raw_ostream &stream, fitx::Instruction *Inst,
                   std::string warn) {
  stream << "[ERROR] ";
  stream << getDebugInfo(Inst);
  stream << warn << "\n";
}

void generateWarning(fitx::BasicBlock *basic_block, std::string warn) {
  if (!Debug)
    return;
  llvm::errs() << "[WARNING] ";
  llvm::errs() << basic_block->Line() << ":";
  llvm::errs() << basic_block->Name() << ": ";
  llvm::errs() << warn << "\n";
}

void generateError(llvm::raw_ostream &stream, fitx::Instruction *Inst,
                   fitx::Value *value) {
  stream << "[ERROR] ";
  stream << getDebugInfo(Inst);
  stream << *value << "\n";
}

void generateLog(llvm::raw_ostream &stream, fitx::Instruction *Inst,
                 std::string warn) {
  stream << "  [LOG] ";
  stream << getDebugInfo(Inst);
  stream << warn << "\n";
}

std::string getDebugInfo(fitx::Instruction *inst) {
  if (const llvm::DebugLoc &Loc = inst->getDebugLoc()) {
    unsigned line = Loc.getLine();
    unsigned col = Loc.getCol();
    return std::string(Loc->getFilename()) + ":" + std::to_string(line) + ":" +
           std::to_string(col) + ": ";
  }
  return std::string();
}
}; // namespace fitx
