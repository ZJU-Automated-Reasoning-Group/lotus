#include "Checker/FiTx/Core/Instruction.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Value.h"

namespace fitx {
std::shared_ptr<Instruction>
Instruction::Create(std::shared_ptr<Instruction> inst,
                    std::vector<Fields> field, long array_element_num) {
  auto created =
      Converter::GetInstance().createManagedInst<fitx::Instruction>(
          inst, array_element_num, field);
  return created;
}

Instruction::Instruction()
    : llvm_instruction_(nullptr), module_(nullptr), opcode_(0), line_(0),
      Value() {}

Instruction::Instruction(llvm::Instruction *instruction)
    : llvm_instruction_(instruction), Value(instruction) {
  module_ = llvm_instruction_->getFunction()->getParent();
  opcode_ = llvm_instruction_->getOpcode();
  if (const llvm::DebugLoc &Loc = llvm_instruction_->getDebugLoc()) {
    debug_loc_ = Loc;
    line_ = Loc.getLine();
    column_ = Loc.getCol();
  }
}

Instruction::Instruction(unsigned int opcode, llvm::Instruction *instruction)
    : opcode_(opcode), llvm_instruction_(instruction), Value(instruction) {
  module_ = llvm_instruction_->getFunction()->getParent();
  if (const llvm::DebugLoc &Loc = llvm_instruction_->getDebugLoc()) {
    debug_loc_ = Loc;
    line_ = Loc.getLine();
    column_ = Loc.getCol();
  }
}

Instruction::Instruction(llvm::Instruction *instruction,
                         std::vector<Fields> fields, long array_element_num)
    : Value(instruction, fields, array_element_num),
      llvm_instruction_(instruction) {
  module_ = llvm_instruction_->getFunction()->getParent();
  opcode_ = llvm_instruction_->getOpcode();

  llvm::BasicBlock *block = llvm_instruction_->getParent();
  parent_ = fitx::Function::createManagedFunction(block->getParent())
                ->getBasicBlock(block);

  if (const llvm::DebugLoc &Loc = llvm_instruction_->getDebugLoc()) {
    debug_loc_ = Loc;
    line_ = Loc.getLine();
    column_ = Loc.getCol();
  }
}

Instruction::Instruction(std::shared_ptr<Instruction> instruction,
                         std::vector<Fields> fields, long array_element_num)
    : Value(instruction, fields, array_element_num),
      llvm_instruction_(instruction->LLVMInstruction()) {
  module_ = const_cast<llvm::Module *>(instruction->Module());
  opcode_ = instruction->Opcode();
  debug_loc_ = instruction->getDebugLoc();

  line_ = instruction->Line();
  column_ = instruction->Column();
  parent_ = instruction->Parent();
}

bool Instruction::operator<=(const fitx::Instruction &instruction) const {
  if (llvm_instruction_ == instruction.llvm_instruction_)
    return true;

  return module_ == instruction.module_ && line_ == instruction.line_ ||
         *this < instruction;
}

bool Instruction::operator<(llvm::Instruction *instruction) const {
  if (const llvm::DebugLoc &Loc = llvm_instruction_->getDebugLoc()) {
    if (module_ != instruction->getFunction()->getParent())
      return module_ < instruction->getFunction()->getParent();

    if (line_ != Loc.getLine())
      return line_ < Loc.getLine();

    return opcode_ < instruction->getOpcode();
  }
  return true;
}

bool Instruction::operator<(const fitx::Instruction &instruction) const {
  if (module_ != instruction.module_)
    return module_ < instruction.module_;

  if (line_ != instruction.line_)
    return line_ < instruction.line_;

  return opcode_ < instruction.opcode_;
}

bool Instruction::operator==(llvm::Instruction *instruction) const {
  if (llvm_instruction_ == instruction)
    return true;

  if (const llvm::DebugLoc &Loc = llvm_instruction_->getDebugLoc())
    return module_ == instruction->getFunction()->getParent() &&
           line_ == Loc.getLine() && opcode_ == instruction->getOpcode();

  return false;
}

bool Instruction::operator==(const fitx::Instruction &instruction) const {
  if (llvm_instruction_ == instruction.llvm_instruction_)
    return true;

  return module_ == instruction.module_ && opcode_ == instruction.opcode_ &&
         line_ == instruction.line_;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              const fitx::Instruction &instruction) {
  ostream << "[fitx::Instruction] ";
  ostream << llvm::Instruction::getOpcodeName(instruction.opcode_);
  ostream << " (" << instruction.opcode_ << ") ";
  ostream << "in " << instruction.module_->getName();
  ostream << ":" << instruction.line_ << "\n";

  return ostream;
}

bool Instruction::emptyInstruction() { return !llvm_instruction_; }

bool Instruction::isInSameLine(fitx::Instruction inst) {
  return line_ == inst.Line();
}

} // namespace fitx
