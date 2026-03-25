/*
 *  Author: rainoftime
 *  Date: 2025-04
 *  Description: Adapter interface for DyckAA used by NullPointer analyses
 */

#ifndef NULLPOINTER_ALIASANALYSISADAPTER_H
#define NULLPOINTER_ALIASANALYSISADAPTER_H

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

using namespace llvm;

// Forward declarations
namespace llvm {
class Module;
} // namespace llvm

// Forward declaration for DyckAliasAnalysis
class DyckAliasAnalysis;

// Abstract interface for alias analysis adapters
class AliasAnalysisAdapter {
public:
  virtual ~AliasAnalysisAdapter() = default;

  // Return true if V1 may alias with V2 at the given instruction point
  virtual bool mayAlias(Value *V1, Value *V2, Instruction *InstPoint,
                        bool IncludeI = true) = 0;

  // Return true if V may be null at the given instruction point
  virtual bool mayNull(Value *V, Instruction *InstPoint,
                       bool BeforeInstruction = false) = 0;

  // Factory method to create the appropriate adapter
  // The caller takes ownership of the returned object
  static AliasAnalysisAdapter *createAdapter(Module *M,
                                             const DyckAliasAnalysis *DAA);
};

// Adapter for DyckAA
class DyckAAAdapter : public AliasAnalysisAdapter {
private:
  Module *ModuleRef;
  const DyckAliasAnalysis *DyckAA;

public:
  explicit DyckAAAdapter(Module *M, const DyckAliasAnalysis *DAA);
  ~DyckAAAdapter();

  bool mayAlias(Value *V1, Value *V2, Instruction *InstPoint,
                bool IncludeI = true) override;

  bool mayNull(Value *V, Instruction *InstPoint,
               bool BeforeInstruction = false) override;
};

#endif // NULLPOINTER_ALIASANALYSISADAPTER_H