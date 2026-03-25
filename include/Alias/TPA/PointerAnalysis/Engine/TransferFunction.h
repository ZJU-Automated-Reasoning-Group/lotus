#pragma once

#include "Alias/TPA/PointerAnalysis/Engine/EvalResult.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFGNode.h"

namespace context {
class Context;
} // namespace context

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace annotation {
class APosition;
class CopyDest;
class CopySource;
class PointerAllocEffect;
class PointerCopyEffect;
class PointerEffect;
} // namespace annotation

namespace tpa {

class FunctionContext;
class GlobalState;
class Pointer;
class ProgramPoint;

// Transfer function evaluator for pointer analysis
//
// Transfer functions model the effect of each CFG node on the points-to
// information. The eval() method takes a program point and returns how the
// analysis state changes.
//
// Transfer Function Types:
// - Entry: Initialize function parameters
// - Alloc: Create new memory objects
// - Copy: Pointer assignment (p = q)
// - Offset: Address-of operation (p = &obj.field)
// - Load: Dereference (p = *q)
// - Store: Store through pointer (*p = q)
// - Call: Function call handling
// - Return: Return value propagation
//
// The evaluator produces EvalResult containing:
// - Updated Store (memory-level points-to information)
// - Successor program points to add to worklist
//
// Important split used throughout TPA:
// - Alloc/Copy/Offset are propagated as top-level-only successors.
// - Entry/Load/Store/Call/Ret are evaluated with local Store state.
class TransferFunction {
private:
  // Shared analysis state used by transfer evaluation.
  GlobalState &globalState;

  // The local store state at this program point
  const Store *localState;

  // Helper methods for building successor lists
  void addTopLevelSuccessors(const ProgramPoint &, EvalResult &);
  void addMemLevelSuccessors(const ProgramPoint &, const Store &, EvalResult &);

  // Allocation transfer function helpers
  bool evalMemoryAllocation(const context::Context *, const llvm::Instruction *,
                            const TypeLayout *, bool);
  // Offset transfer function helpers
  bool copyWithOffset(const Pointer *, const Pointer *, size_t, bool);
  PtsSet offsetMemory(const MemoryObject *, size_t, bool);
  // Load transfer function helpers
  PtsSet loadFromPointer(const Pointer *, const Store &);
  // Store transfer function helpers
  void evalStore(const Pointer *, const Pointer *, const ProgramPoint &,
                 EvalResult &);
  void strongUpdateStore(const MemoryObject *, PtsSet, Store &);
  void weakUpdateStore(PtsSet, PtsSet, Store &);
  // Call transfer function helpers
  std::vector<const llvm::Function *> findFunctionInPtsSet(PtsSet,
                                                           const CallCFGNode &);
  std::vector<const llvm::Function *>
  resolveCallTarget(const context::Context *, const CallCFGNode &);
  std::vector<PtsSet> collectArgumentPtsSets(const context::Context *,
                                             const CallCFGNode &, size_t);
  bool updateParameterPtsSets(const FunctionContext &,
                              const std::vector<PtsSet> &);
  std::pair<bool, bool> evalCallArguments(const context::Context *,
                                          const CallCFGNode &,
                                          const FunctionContext &);
  void evalExternalCall(const context::Context *, const CallCFGNode &,
                        const FunctionContext &, EvalResult &);
  void evalInternalCall(const context::Context *, const CallCFGNode &,
                        const FunctionContext &, EvalResult &, bool);
  // Return transfer function helpers
  std::pair<bool, bool> evalReturnValue(const context::Context *,
                                        const ReturnCFGNode &,
                                        const ProgramPoint &);
  void evalReturn(const context::Context *, const ReturnCFGNode &,
                  const ProgramPoint &, EvalResult &);
  // External call effect helpers
  bool evalMallocWithSize(const context::Context *, const llvm::Instruction *,
                          llvm::Type *, const llvm::Value *);
  bool evalExternalAlloc(const context::Context *, const CallCFGNode &,
                         const annotation::PointerAllocEffect &);
  void evalMemcpyPtsSet(const MemoryObject *,
                        const std::vector<const MemoryObject *> &, size_t,
                        Store &);
  bool evalMemcpyPointer(const Pointer *, const Pointer *, Store &);
  bool evalMemcpy(const context::Context *, const CallCFGNode &, Store &,
                  const annotation::APosition &, const annotation::APosition &);
  void fillPtsSetWith(const Pointer *, PtsSet, Store &);
  PtsSet evalExternalCopySource(const context::Context *, const CallCFGNode &,
                                const annotation::CopySource &);
  void evalExternalCopyDest(const context::Context *, const CallCFGNode &,
                            EvalResult &, const annotation::CopyDest &, PtsSet);
  void evalExternalCopy(const context::Context *, const CallCFGNode &,
                        EvalResult &, const annotation::PointerCopyEffect &);
  void evalExternalCallByEffect(const context::Context *, const CallCFGNode &,
                                const annotation::PointerEffect &,
                                EvalResult &);

  // Node-specific evaluation methods
  void evalEntryNode(const ProgramPoint &, EvalResult &);
  void evalAllocNode(const ProgramPoint &, EvalResult &);
  void evalCopyNode(const ProgramPoint &, EvalResult &);
  void evalOffsetNode(const ProgramPoint &, EvalResult &);
  void evalLoadNode(const ProgramPoint &, EvalResult &);
  void evalStoreNode(const ProgramPoint &, EvalResult &);
  void evalCallNode(const ProgramPoint &, EvalResult &);
  void evalReturnNode(const ProgramPoint &, EvalResult &);

public:
  // Constructor
  // Parameters: g - global state, s - initial store
  TransferFunction(GlobalState &g, const Store *s)
      : globalState(g), localState(s) {}

  // Evaluate the transfer function for a program point
  // Parameters: pp - the program point to evaluate
  // Returns: EvalResult containing updated store and successors
  EvalResult eval(const ProgramPoint &);
};

} // namespace tpa
