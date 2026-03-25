#pragma once

#include <string>
#include <vector>

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

enum class AEBugType {
  BUFFER_OVERFLOW,
  NULLPTR_DEREF,
  USE_AFTER_FREE,
  UNINITIALIZED_USE,
  INVALID_FREE
};

struct AEBugDiagStep {
  const llvm::Instruction *inst;
  std::string tip;
  std::string src_file;
  unsigned src_line;
  unsigned src_column;
  std::string func_name;
  std::string llvm_ir;

  AEBugDiagStep(const llvm::Instruction *I, const std::string &t)
      : inst(I), tip(t), src_line(0), src_column(0) {
    if (inst) {
      if (inst->getDebugLoc()) {
        src_line = inst->getDebugLoc().getLine();
        src_column = inst->getDebugLoc().getCol();
      }
      if (inst->getFunction()) {
        func_name = inst->getFunction()->getName().str();
      }
      std::string ir;
      llvm::raw_string_ostream rso(ir);
      inst->print(rso);
      llvm_ir = rso.str();
    }
  }
};

class AEBugReport {
public:
  AEBugReport(AEBugType type, const std::string &description)
      : bugType(type), description(description) {}

  void addDiagStep(const llvm::Instruction *inst, const std::string &tip) {
    diagSteps.emplace_back(inst, tip);
  }

  void emit() const {
    llvm::errs() << "=== Bug Report ===\n";
    llvm::errs() << "Type: ";
    switch (bugType) {
    case AEBugType::BUFFER_OVERFLOW:
      llvm::errs() << "Buffer Overflow\n";
      break;
    case AEBugType::NULLPTR_DEREF:
      llvm::errs() << "Null Pointer Dereference\n";
      break;
    case AEBugType::USE_AFTER_FREE:
      llvm::errs() << "Use After Free\n";
      break;
    case AEBugType::UNINITIALIZED_USE:
      llvm::errs() << "Uninitialized Use\n";
      break;
    case AEBugType::INVALID_FREE:
      llvm::errs() << "Invalid Free\n";
      break;
    }
    llvm::errs() << "Description: " << description << "\n";
    llvm::errs() << "Trace:\n";
    for (const auto &step : diagSteps) {
      llvm::errs() << "  - " << step.tip << "\n";
      if (!step.src_file.empty()) {
        llvm::errs() << "    Location: " << step.src_file << ":"
                     << step.src_line << ":" << step.src_column << "\n";
      }
      if (!step.func_name.empty()) {
        llvm::errs() << "    Function: " << step.func_name << "\n";
      }
    }
    llvm::errs() << "===============\n\n";
  }

  const std::string &getDescription() const { return description; }
  const std::vector<AEBugDiagStep> &getDiagSteps() const { return diagSteps; }

private:
  AEBugType bugType;
  std::string description;
  std::vector<AEBugDiagStep> diagSteps;
};

} // namespace analysis
} // namespace lotus
