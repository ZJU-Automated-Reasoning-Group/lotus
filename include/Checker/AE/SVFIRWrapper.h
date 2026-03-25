//===- SVFIRWrapper.h -- SVFIR-like interface using AserPTA ----------//
//
// Migrated from SVF's AE engine to Lotus.
// Provides SVFIR-like interface using AserPTA for pointer analysis.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace analysis {

class SVFIRWrapper {
public:
  SVFIRWrapper(void *ptaSolver, llvm::Module *module);
  ~SVFIRWrapper();

  bool isPTAReady() const { return ptaSolver_ != nullptr; }

  // Get points-to set for a value using AserPTA
  void getPointsTo(const llvm::Value *V, std::vector<void *> &result) const;

  // Get object type from pointer
  const llvm::Type *getObjectType(const llvm::Value *V) const;

  // Check alias between two pointers
  bool alias(const llvm::Value *v1, const llvm::Value *v2) const;

  // Get call graph
  void *getCallGraph() const { return ptaSolver_; }

  // Check if value is a pointer
  bool isPointerType(const llvm::Value *V) const {
    return V && V->getType()->isPointerTy();
  }

  // Get function by name
  const llvm::Function *getFunction(const std::string &name) const;

  // Get the module being analyzed
  llvm::Module *getModule() const { return module_; }

  // Get the PTA solver
  void *getPTASolver() const { return ptaSolver_; }

  // Get object size in bytes using LLVM type information
  // This uses the allocation site's type to determine size
  uint32_t getByteSizeOfObj(const void *obj) const;

  // Get the LLVM Value associated with an object
  const llvm::Value *getObjValue(const void *obj) const;

private:
  void *ptaSolver_;
  llvm::Module *module_;
};

} // namespace analysis
} // namespace lotus
