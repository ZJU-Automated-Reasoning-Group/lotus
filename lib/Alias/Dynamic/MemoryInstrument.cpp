#include "Alias/Dynamic/MemoryInstrument.h"

#include "Alias/Dynamic/AllocType.h"
#include "Alias/Dynamic/DynamicHooks.h"
#include "Alias/Dynamic/FeatureCheck.h"
#include "Alias/Dynamic/IDAssigner.h"
#include "Alias/Spec/AliasSpecManager.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace dynamic {

namespace {

/// Returns the insertion point immediately after an instruction
BasicBlock::iterator nextInsertionPos(Instruction &inst) {
  BasicBlock::iterator loc(&inst);
  ++loc;
  return loc;
}

/// Instruments LLVM IR with runtime hooks to track memory operations and
/// pointer assignments
class Instrumenter {
private:
  DynamicHooks &hooks;     ///< Hook function declarations
  const IDAssigner &idMap; ///< Maps LLVM values to unique IDs
  const lotus::alias::AliasSpecManager
      &specMgr; ///< Identifies allocator functions

  LLVMContext &context;

  size_t getID(const Value &v) const {
    const auto *id = idMap.getID(v);
    assert(id != nullptr && "ID not found");
    return *id;
  }
  /// Helper functions to create LLVM types for hook arguments
  Type *getIntType() { return Type::getIntNTy(context, sizeof(int) * 8); }
  Type *getLongType() { return Type::getIntNTy(context, sizeof(size_t) * 8); }
  Type *getCharType() { return Type::getInt8Ty(context); }
  Type *getCharPtrType() { return PointerType::getUnqual(getCharType()); }

  void instrumentPointer(Value *, Instruction *);
  void instrumentAllocation(AllocType, Value *, Instruction *);
  void instrumentGlobals(Module &);
  void instrumentFunction(Function &);
  void instrumentFunctionParams(Function &);
  void instrumentMain(Function &);
  void instrumentEntry(Function &);

  void instrumentInst(Instruction &);
  void instrumentExit(Instruction &);
  void instrumentAlloca(AllocaInst &);
  void instrumentCallInst(CallInst *CI);
  void instrumentInvokeInst(InvokeInst *II);
  void instrumentPointerInst(Instruction &);
  void instrumentMalloc(Instruction *inst);

public:
  Instrumenter(DynamicHooks &d, const IDAssigner &i,
               const lotus::alias::AliasSpecManager &s, LLVMContext &c)
      : hooks(d), idMap(i), specMgr(s), context(c) {}

  void instrument(Module &);
};

/// Inserts a hook call to log a pointer assignment
void Instrumenter::instrumentPointer(Value *val, Instruction *pos) {
  assert(val != nullptr && pos != nullptr && val->getType()->isPointerTy());
  auto id = getID(*val);

  auto *idArg = ConstantInt::get(getIntType(), id);
  if (val->getType() != getCharPtrType())
    val = new BitCastInst(val, getCharPtrType(), "ptr", pos);
  CallInst::Create(hooks.getPointerHook(), {idArg, val}, "", pos);
}

/// Inserts a hook call to log a memory allocation (global, stack, or heap)
void Instrumenter::instrumentAllocation(AllocType allocType, Value *ptr,
                                        Instruction *pos) {
  assert(ptr != nullptr && pos != nullptr && ptr->getType()->isPointerTy());

  auto *allocTypeArg = ConstantInt::get(getCharType(), allocType);
  const auto ptrId = getID(*ptr);
  if (ptr->getType() != getCharPtrType())
    ptr = new BitCastInst(ptr, getCharPtrType(), "alloc_ptr", pos);
  auto *idArg = ConstantInt::get(getIntType(), ptrId);
  CallInst::Create(hooks.getAllocHook(), {allocTypeArg, idArg, ptr}, "", pos);
}

/// Instruments all global variables and functions as global allocations
void Instrumenter::instrumentGlobals(Module &module) {
  auto *bb = BasicBlock::Create(context, "entry", hooks.getGlobalHook());
  auto *retInst = ReturnInst::Create(context, bb);

  // Global values
  for (auto &global : module.globals()) {
    // Prevent global variables from sharing the same address, because it
    // breaks the assumption that global variables do not alias.
    if (global.hasAtLeastLocalUnnamedAddr())
      global.setUnnamedAddr(GlobalValue::UnnamedAddr::None);

    instrumentAllocation(AllocType::Global, &global, retInst);
  }

  // Functions
  for (auto &f : module) {
    if (f.isIntrinsic())
      continue;
    if (hooks.isHook(f))
      continue;

    if (f.hasAtLeastLocalUnnamedAddr())
      f.setUnnamedAddr(GlobalValue::UnnamedAddr::None);

    instrumentAllocation(AllocType::Global, &f, retInst);
  }
}

/// Instruments function parameters: logs byval allocations and pointer
/// assignments
void Instrumenter::instrumentFunctionParams(Function &f) {
  auto entry = f.begin()->getFirstInsertionPt();
  for (auto &arg : f.args()) {
    if (arg.getType()->isPointerTy()) {
      if (arg.hasByValAttr()) {
        instrumentAllocation(AllocType::Stack, &arg, &*entry);
      }

      instrumentPointer(&arg, &*entry);
    }
  }
}

/// Inserts a function entry hook at the beginning of the function
void Instrumenter::instrumentEntry(Function &f) {
  auto entry = f.begin()->getFirstInsertionPt();
  const auto id = getID(f);
  auto *idArg = ConstantInt::get(getIntType(), id);
  CallInst::Create(hooks.getEnterHook(), {idArg}, "", &*entry);
}

/// Inserts a function exit hook before a return instruction
void Instrumenter::instrumentExit(Instruction &inst) {
  auto *func = inst.getParent()->getParent();
  auto funcID = getID(*func);
  auto *idArg = ConstantInt::get(getIntType(), funcID);
  CallInst::Create(hooks.getExitHook(), {idArg}, "", &inst);
}

/// Instruments stack allocations (alloca instructions)
void Instrumenter::instrumentAlloca(AllocaInst &allocInst) {
  auto pos = nextInsertionPos(allocInst);
  instrumentAllocation(AllocType::Stack, &allocInst, &*pos);
}

/// Instruments heap allocations (malloc, calloc, etc.)
void Instrumenter::instrumentMalloc(Instruction *inst) {
  auto pos = nextInsertionPos(*inst);
  instrumentAllocation(AllocType::Heap, inst, &*pos);
}

/// Instruments call instructions: logs allocators and regular calls, tracks
/// return values
void Instrumenter::instrumentCallInst(CallInst *CI) {
  // Instrument memory allocation function calls.
  auto *callee = CI->getCalledFunction();

  if (callee && specMgr.isAllocator(callee))
    instrumentMalloc(CI);
  else {
    // CallHook must be inserted before the call actually happens
    const auto id = getID(*CI);
    auto *idArg = ConstantInt::get(getIntType(), id);
    CallInst::Create(hooks.getCallHook(), {idArg}, "", CI);

    // If the call returns a pointer, record it
    if (CI->getType()->isPointerTy())
      instrumentPointer(CI, &*nextInsertionPos(*CI));
  }
}

/// Instruments invoke instructions (exception-handling calls): similar to call
/// instructions
void Instrumenter::instrumentInvokeInst(InvokeInst *II) {
  // Instrument memory allocation function calls.
  auto *callee = II->getCalledFunction();

  if (callee && specMgr.isAllocator(callee))
    instrumentMalloc(II);
  else {
    // CallHook must be inserted before the call actually happens
    const auto id = getID(*II);
    auto *idArg = ConstantInt::get(getIntType(), id);
    CallInst::Create(hooks.getCallHook(), {idArg}, "", II);

    // If the call returns a pointer, record it
    if (II->getType()->isPointerTy()) {
      // For invoke instructions, we need to be careful about insertion point
      // since they have multiple successors
      auto *normalDest = II->getNormalDest();
      auto *firstInst = &*normalDest->getFirstInsertionPt();
      instrumentPointer(II, firstInst);
    }
  }
}

/// Instruments pointer-producing instructions, handling PHI nodes specially
void Instrumenter::instrumentPointerInst(Instruction &inst) {
  if (isa<PHINode>(inst)) {
    // Cannot insert hooks right after a PHI, because PHINodes have to be
    // grouped together
    auto *pos = inst.getParent()->getFirstNonPHI();
    instrumentPointer(&inst, &*pos);
  } else if (!inst.isTerminator()) {
    auto pos = nextInsertionPos(inst);
    instrumentPointer(&inst, &*pos);
  }
}

/// Instruments a single instruction based on its type
void Instrumenter::instrumentInst(Instruction &inst) {
  // Do not touch the instrumented codes
  if (idMap.getID(inst) == nullptr)
    return;

  if (inst.getType()->isPointerTy())
    instrumentPointerInst(inst);

  if (auto *allocInst = dyn_cast_or_null<AllocaInst>(&inst))
    instrumentAlloca(*allocInst);
  else if (auto *callInst = dyn_cast_or_null<CallInst>(&inst))
    instrumentCallInst(callInst);
  else if (auto *invokeInst = dyn_cast_or_null<InvokeInst>(&inst))
    instrumentInvokeInst(invokeInst);
  else if (isa<StoreInst>(inst))
    return;
  else if (isa<ReturnInst>(inst))
    instrumentExit(inst);
}

/// Special instrumentation for main(): initializes logging and handles
/// argv/envp
void Instrumenter::instrumentMain(Function &mainFunc) {
  assert(mainFunc.getName() == "main");

  auto pos = mainFunc.begin()->getFirstInsertionPt();
  CallInst::Create(hooks.getInitHook(), "", &*pos);
  CallInst::Create(hooks.getGlobalHook(), "", &*pos);

  if (mainFunc.arg_size() > 0) {
    assert(mainFunc.arg_size() >= 2);
    auto *argItr = mainFunc.arg_begin();
    ++argItr;
    auto *argv = &*argItr;
    auto argvId = getID(*argv);
    auto *idArg = ConstantInt::get(getIntType(), argvId);

    Value *envp = ConstantPointerNull::get(cast<PointerType>(getCharPtrType()));
    Constant *envpArg = ConstantInt::get(getIntType(), 0);
    ++argItr;
    if (argItr != mainFunc.arg_end()) {
      envp = &*argItr;
      auto envpId = getID(*envp);
      envpArg = ConstantInt::get(getIntType(), envpId);
    }

    CallInst::Create(hooks.getMainHook(), {idArg, argv, envpArg, envp}, "",
                     &*pos);
  }

  const auto id = getID(mainFunc);
  auto *idArg = ConstantInt::get(getIntType(), id);
  CallInst::Create(hooks.getEnterHook(), {idArg}, "", &*pos);
}

/// Instruments a function: processes all instructions and adds entry/exit hooks
void Instrumenter::instrumentFunction(Function &f) {
  if (f.isDeclaration())
    return;

  if (hooks.isHook(f))
    return;

  for (auto &bb : f)
    for (auto &inst : bb)
      instrumentInst(inst);

  if (f.getName() == "main")
    instrumentMain(f);
  else {
    instrumentFunctionParams(f);
    instrumentEntry(f);
  }
}

/// Main instrumentation entry point: instruments globals and all functions
void Instrumenter::instrument(Module &module) {
  instrumentGlobals(module);

  for (auto &f : module)
    instrumentFunction(f);
}
} // namespace

/// LLVM pass entry point: checks features, assigns IDs, and instruments the
/// module
void MemoryInstrument::runOnModule(llvm::Module &module) {
  // Check unsupported features in the input IR and issue warnings accordingly
  FeatureCheck().runOnModule(module);

  IDAssigner idMap(module);
  DynamicHooks hooks(module);
  lotus::alias::AliasSpecManager specMgr;
  specMgr.initialize(module);

  Instrumenter(hooks, idMap, specMgr, module.getContext()).instrument(module);
}
} // namespace dynamic
