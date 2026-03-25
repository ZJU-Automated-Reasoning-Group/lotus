// This is the implementation of the --flatten-init pass, a.k.a. "LLVM memory
// folding".  Its purpose is to eliminate non-secret-dependent initialization
// code and produce an equivalent program that begins in a state where the
// initialization has already occurred.
//
// The FlattenInit pass has three main effects:
//
// 1. Starting from the top of `main`, it inlines all function calls that occur
//    prior to the first branch on unknown (secret) data.
// 2. It resolves memory loads by tracking the effects of previous stores.
// 3. It eliminates dynamic allocations by converting them to fresh globals,
//    and eliminates stores by updating global initializers in place.
//
// All three parts interact closely with constant propagation: loads from known
// globals can be converted to constants; stores to constant addresses can be
// tracked precisely to resolve future loads; and resolving branch conditions
// to constants allows precise determination of which code actually runs during
// the public initialization phase.
//
// **Interpreter**: The implementation is structured similar to an abstract
// interpretation, but with the "abstract domain" being LLVM `Value`s in the
// newly-generated optimized `main`.  Each register (function argument or
// instruction result) in the original program and each memory location is
// mapped to a value (usually a constant, but it can also be a new instruction
// result) in the new program.  To "interpret" an instruction from the old
// program, the pass copies the instruction into the new program and replaces
// all its old operands with the new values recorded in the interpreter state.
//
// For example, consider this pseudocode program:
//
// ```Python
// def foo(a, b):
//     x = a - a
//     y = a + b
//     z = x & y
//     if z:
//         w = a + 1
//     else:
//         w = b + 1
//     if w:
//         return 1
//     else:
//         return 2
// ```
//
// The pass defines a new function with the same signature, and maps the old
// arguments to the new ones:
//
// ```Python
// def foo2(a2, b2):
//     # State: a -> a2, b -> b2
// ```
//
// It then begins translating instructions.  It takes the instruction `a - a`,
// substitutes operands to produce `a2 - a2`, and applies simplification,
// resulting in the constant `0`.  Since the result is a constant, the
// interpreter emits no instructions and simply records that constant as the
// value of `x`:
//
// ```Python
//     # State: a -> a2, b -> b2, x -> 0
// ```
//
// The instruction `a + b` can't be folded, so it is emitted instead, and a
// mapping from the old result to the new one is recorded:
//
// ```Python
//     y2 = a2 + b2
//     # State: a -> a2, b -> b2, x -> 0, y -> y2
// ```
//
// Finally, `x & y` is substituted to produce `0 & y2`, which simplifies to the
// constant 0:
//
// ```Python
//     # State: a -> a2, b -> b2, x -> 0, y -> y2, z -> 0
// ```
//
// At the `if z` conditional, substitution produces `if 0`, so this is not a
// branch on unknown data.  The interpreter simply proceeds into the false
// case and ignores the true case.  The instruction `b + 1` is substituted to
// produce `b2 + 1`, and that instruction is emitted and recorded in the map:
//
// ```Python
//     w2 = b2 + 1
//     # State: a -> a2, b -> b2, x -> 0, y -> y2, z -> 0, w -> w2
// ```
//
// Finally, the `if w` conditional becomes `if w2`, which is a branch on an
// unknown value.  The FlattenInit pass stops upon reaching such an
// instruction.
//
// **Function calls**: When the interpreter encounters a function call, it
// pushes a new stack frame, as in a normal interpreter.  The new program is
// not affected in any special way; instructions from the callee are emitted
// into the new program just as instructions from the caller were, with no
// special distinction between the two.  This means that new-program values can
// simply be copied into the new stack frame when passing arguments, and
// similarly for return values.
//
// When the interpreter encounters an unsupported instruction (such as a branch
// on unknown data), it stops interpreting immediately.  Instead of returning
// normally, it begins unwinding the interpreter stack.  At each level of the
// stack, it emits into the new program a copy of all parts of the (old)
// function that are reachable from the current program counter.  For example,
// given this code:
//
// ```Python
// def f():
//     x = 1
//     g()
//     y = 2
//
// def g():
//     a = 3
//     if unknown:
//         b = 4
//     c = 5
// ```
//
// Upon reaching `if unknown`, the interpreter will stop.  It will first emit
// the remainder of `g`, then pop `g`'s stack frame, and finally emit the
// remainder of `f`.  The final program will look something like this (ignoring
// the handling of constant RHS expressions, to keep the example simple):
//
// ```Python
// def f2():
//     x = 1
//     a = 3
//     # Unwinding happens here
//     # Remainder of g:
//     if unknown:
//         b = 4
//     c = 5
//     # Remainder of f:
//     y = 2
// ```
//
// **Memory**: The interpreter models memory as a map from block ID to
// contents.  The block ID is an LLVM `Value`, either a `GlobalReference` to a
// global variable or an `alloca` or `malloc`-call `Instruction` (in the new
// program).  The block contents maps each offset to the most recent write to
// that offset.  For global variables, the block contents are initialized using
// the global's initializer.
//
// When processing instructions, the interpreter can eliminate a load
// instruction with a constant address by retrieving the value at that address
// from its own representation of memory.  Specifically, for `x = load(a)`
// where `a` maps to a constant `c`, instead of emitting `x2 = load(c)` and
// recording `x -> x2`, the interpreter can retrieve the actual value `v` at
// address `c`, emit no instructios, and record `x -> v`.
//
// The interpreter can eliminate many store instructions as well.  If the first
// memory operation of the new program was a store of a constant value at a
// constant address, we could modify the initializer of the global being
// written to apply the store instruction's effect and then eliminate the
// instruction itself.  The interpreter does this: as long as it hasn't emitted
// any memory operations yet, it attempts to fold the effect of each store into
// the global initializers so it accan avoid emitting the actual store
// instruction.  It also handles `malloc` and `alloca` instructions by
// replacing them with fresh globals, after which stores to these dynamic
// allocations can be handled the same way.
//
// For efficiency, the interpreter does not actually update global initializers
// incrementally as it steps through the program.  Instead, it only updates its
// internal representation of the state of memory, and before it emits an
// unhandled store or begins unwinding, it updates all global initializers to
// match the internal representation.

#include "llvm/ADT/BitVector.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

/// A single LinearPtr term, consisting of a pointer times some coefficient.
struct LinearTerm {
  /// The pointer, represented as an LLVM value.  This should either be a
  /// GlobalReference constant or an allocation Instruction such as `alloca` or
  /// a call to `malloc`.
  Value *Ptr;
  /// The coefficient to multiply `Ptr` by.  Addition, subtraction, and
  /// multiplication all work the same regardless of the signedness of the
  /// inputs, so for simplicity we make the coefficient unsigned.
  uint64_t Coeff;

  LinearTerm(Value *Ptr, uint64_t Coeff) : Ptr(Ptr), Coeff(Coeff) {}
};

void addTerm(LinearTerm Term, SmallVector<LinearTerm, 1> &Terms) {
  for (auto &OldTerm : Terms) {
    if (OldTerm.Ptr == Term.Ptr) {
      OldTerm.Coeff += Term.Coeff;
      if (OldTerm.Coeff == 0) {
        std::swap(OldTerm, Terms.back());
        Terms.pop_back();
      }
      return;
    }
  }

  // No term with matching `Ptr` was found in `Terms`.
  Terms.push_back(Term);
}

void mergeTerms(SmallVector<LinearTerm, 1> &Dest,
                SmallVector<LinearTerm, 1> const &Src) {
  DenseMap<Value *, unsigned> PtrIdx;
  for (unsigned I = 0; I < Dest.size(); ++I) {
    PtrIdx.insert(std::make_pair(Dest[I].Ptr, I));
  }
  for (auto const &Term : Src) {
    auto It = PtrIdx.find(Term.Ptr);
    if (It != PtrIdx.end()) {
      unsigned I = It->second;
      Dest[I].Coeff += Term.Coeff;
      if (Dest[I].Coeff == 0) {
        std::swap(Dest[I], Dest.back());
        Dest.pop_back();
        PtrIdx.erase(It);
        PtrIdx.insert(std::make_pair(Dest[I].Ptr, I));
      }
    } else {
      Dest.push_back(Term);
    }
  }
}

/// A linear combination of pointers.  This is used when evaluating addresses,
/// in order to handle complex pointer expressions like `ptr + (end - ptr)`.
struct LinearPtr {
  SmallVector<LinearTerm, 1> Terms;
  uint64_t Offset;

  LinearPtr() : Terms(), Offset(0) {}
  explicit LinearPtr(Value *Ptr) : Terms{LinearTerm(Ptr, 1)}, Offset(0) {}
  explicit LinearPtr(uint64_t Const) : Terms{}, Offset(Const) {}

  LinearPtr neg() const {
    LinearPtr Out = *this;
    Out.Offset = -Out.Offset;
    for (auto &Term : Out.Terms) {
      Term.Coeff = -Term.Coeff;
    }
    return std::move(Out);
  }

  LinearPtr add(const LinearPtr &Other) const {
    LinearPtr Out;
    Out.Offset = Offset + Other.Offset;

    if (Terms.size() == 1) {
      Out.Terms = Other.Terms;
      addTerm(Terms[0], Out.Terms);
    } else if (Other.Terms.size() == 1) {
      Out.Terms = Terms;
      addTerm(Other.Terms[0], Out.Terms);
    } else {
      Out.Terms = Terms;
      mergeTerms(Out.Terms, Other.Terms);
    }

    return std::move(Out);
  }

  LinearPtr sub(const LinearPtr &Other) const {
    return std::move(add(Other.neg()));
  }

  /// Multiply this pointer expression by a constant.
  LinearPtr mulConst(uint64_t Factor) const {
    LinearPtr Out = *this;
    Out.Offset *= Factor;
    for (auto &Term : Out.Terms) {
      Term.Coeff *= Factor;
    }
    return std::move(Out);
  }

  /// Multiply this pointer expression by another pointer expression.  This
  /// fails if it would produce a nonlinear expression, such as `ptr * ptr`.
  Optional<LinearPtr> mul(const LinearPtr &Other) const {
    if (Terms.size() > 0 && Other.Terms.size() > 0) {
      // Non-linear multiplication.
      return None;
    }

    if (Terms.size() == 0) {
      return Other.mulConst(Offset);
    } else {
      return mulConst(Other.Offset);
    }
  }

  /// Try to interpret this as a base-plus-offset expression, and return the
  /// base.  The offset is the value of the `this->Offset` field.  A base of
  /// `nullptr` indicates that no pointers are involved in this expression
  /// (it's an absolute memory address instead).
  Optional<Value *> getBase() const {
    if (Terms.size() == 0) {
      return nullptr;
    } else if (Terms.size() == 1 && Terms[0].Coeff == 1) {
      return Terms[0].Ptr;
    } else {
      return None;
    }
  }
};

typedef uint8_t Label;

const Label BOTTOM = 3;
const Label MAYBE_TAINTED = 2;
const Label LABEL_MASK = 3;
const SmallVector<Label, 8> EmptyLabels;
const SmallVector<Label, 8> MaybeTaintedLabels(8, MAYBE_TAINTED);

enum MemStoreKind {
  /// Store the value `Val` at this location.
  OpStore,
  /// Set `Len` bytes to zero, starting from `Offset`.  This represents a
  /// `memset` call.
  OpZero,
  /// Set `Len` bytes to unknown values, starting from `Offset`.  `memcpy` sets
  /// the affected range to unknown, then copies over all known values from the
  /// source; this represents the fact that copying unknown parts of the source
  /// writes unknown data to the dest.
  OpUnknown,
};

/// A single store operation, which modifies some address range within the
/// containing region.
struct MemStore {
  MemStoreKind Kind;
  uint64_t Offset;
  union {
    /// The value stored, for `OpStore`.
    Value *Val;
    /// The number of bytes affected, for `OpZero` and `OpUnknown`.
    uint64_t Len;
  };
  SmallVector<Label, 8> Labels;

  static MemStore CreateStore(uint64_t Offset, Value *Val,
                              SmallVector<Label, 8> Labels) {
    MemStore MS = {OpStore, Offset, 0, std::move(Labels)};
    MS.Val = Val;
    return MS;
  }

  static MemStore CreateZero(uint64_t Offset, uint64_t Len) {
    MemStore MS = {OpZero, Offset};
    MS.Len = Len;
    return MS;
  }

  static MemStore CreateUnknown(uint64_t Offset, uint64_t Len) {
    MemStore MS = {OpUnknown, Offset};
    MS.Len = Len;
    return MS;
  }

  void setLabel(uint64_t RelOffset, Label L) {
    if (Labels.size() <= RelOffset) {
      Labels.resize(RelOffset + 1, BOTTOM);
    }
    Labels[RelOffset] = L;
  }

  /// Get the number of bytes affected by this operation.
  uint64_t getStoreSize(DataLayout const &DL) const {
    switch (Kind) {
    case OpStore:
      return DL.getTypeStoreSize(Val->getType());
    case OpZero:
    case OpUnknown:
      return Len;
    default:
      assert(0 && "bad mem op kind");
      return 0;
    }
  }

  uint64_t getEndOffset(DataLayout const &DL) const {
    return Offset + getStoreSize(DL);
  }

  bool overlaps(MemStore const &Other, DataLayout const &DL) const {
    return overlapsRange(Other.Offset, Other.getEndOffset(DL), DL);
  }

  bool overlapsRange(uint64_t Start, uint64_t End, DataLayout const &DL) const {
    return Offset < End && Start < getEndOffset(DL);
  }

  bool contains(uint64_t O, DataLayout const &DL) const {
    return Offset <= O && O < getEndOffset(DL);
  }

  bool containsRange(uint64_t Start, uint64_t End, DataLayout const &DL) const {
    return Offset <= Start && End <= getEndOffset(DL);
  }

  bool containedByRange(uint64_t Start, uint64_t End,
                        DataLayout const &DL) const {
    return Start <= Offset && getEndOffset(DL) <= End;
  }
};

/// The contents of a single memory allocation.
struct MemRegion {
  /// A list of operations performed on this region, in chronological order.
  std::vector<MemStore> Ops;
  /// Smallest offset written within this region.
  uint64_t MinOffset;
  /// Largest offset (inclusive) written within this region.  We use an
  /// inclusive range to avoid problems with wraparound.
  uint64_t MaxOffset;
  /// Set to `true` if there has been at least one write to this region since
  /// it was initialized.
  bool Written;

  MemRegion() : Ops(), MinOffset(0), MaxOffset(0), Written(false) {}

  void pushOp(MemStore Op, DataLayout const &DL) {
    uint64_t End = Op.getEndOffset(DL);
    if (End == Op.Offset) {
      // The size of the op is zero.  Don't actually push anything.
      return;
    }
    if (Ops.size() == 0) {
      MinOffset = Op.Offset;
      MaxOffset = End - 1;
    } else {
      if (Op.Offset < MinOffset) {
        MinOffset = Op.Offset;
      }
      if (End - 1 > MaxOffset) {
        MaxOffset = End - 1;
      }
    }
    Ops.push_back(std::move(Op));
    Written = true;
  }
};

/// A representation of the contents of memory.  This is used to handle memory
/// ops in the interpreter.
struct Memory {
  DataLayout const &DL;
  DenseMap<Value *, MemRegion> Regions;

  Memory(DataLayout const &DL) : DL(DL) {}

  /// Get the `MemRegion` representing the contents of allocation `V`.  `V`
  /// should be either a reference to a global variable or a memory allocation
  /// instruction.
  MemRegion &getRegion(Value *V);
  void initRegion(MemRegion &Region, Value *V);
  void storeConstant(MemRegion &Region, uint64_t Offset, Constant *C);

  /// Load a value of type `T` from `Offset` within region `Base`.  If the
  /// range `Offset .. Offset + Len` (where `Len` is the size in bytes of `T`)
  /// is covered entirely by a single `OpStore`, this returns the value stored
  /// (which may or may not have type `T`); if it's covered entirely by a
  /// single `OpZero`, it returns a zero/null constant of type T.  The second
  /// return value is the byte offset where the requested range begins within
  /// the returned `Value`.  For example, if you store an 8-byte value at
  /// offset 16, then load a 1-byte / value from offset 19 the result of the
  /// `load` will be the stored value and the relative offset 3.  (In the
  /// `OpZero` case, the relative offset is always 0.)
  ///
  /// If the range `Offset .. Offset + Len` is not covered by a single
  /// operation, the returned `Value*` is null.
  std::tuple<Value *, SmallVector<Label, 8>, uint64_t>
  load(Value *Base, uint64_t Offset, Type *T);
  /// Store `V` at `Offset` within region `Base`.
  void store(Value *Base, uint64_t Offset, Value *V, SmallVector<Label, 8>);
  void zero(Value *Base, uint64_t Offset, uint64_t Len);
  void setUnknown(Value *Base, uint64_t Offset, uint64_t Len);
  void clear() { Regions.clear(); }
  /// Set the labe of the byte at `Offset` within region `Base`.
  void setLabel(Value *Base, uint64_t Offset, Label L);
};

MemRegion &Memory::getRegion(Value *V) {
  auto It = Regions.find(V);
  if (It == Regions.end()) {
    MemRegion Region;
    initRegion(Region, V);
    // Don't count the initialization as a write.
    Region.Written = false;
    It = Regions.try_emplace(V, std::move(Region)).first;
  }
  return It->second;
}

void Memory::initRegion(MemRegion &Region, Value *V) {
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    if (!GV->hasInitializer()) {
      return;
    }
    Constant *C = GV->getInitializer();
    storeConstant(Region, 0, C);
  }
}

void Memory::storeConstant(MemRegion &Region, uint64_t Offset, Constant *C) {
  if (auto *Array = dyn_cast<ConstantArray>(C)) {
    Type *ElemTy = Array->getType()->getElementType();
    uint64_t Stride = DL.getTypeAllocSize(ElemTy);
    for (unsigned I = 0; I < Array->getNumOperands(); ++I) {
      storeConstant(Region, Offset + I * Stride, Array->getOperand(I));
    }
  } else if (auto *DataArray = dyn_cast<ConstantDataArray>(C)) {
    Type *ElemTy = DataArray->getElementType();
    uint64_t Stride = DL.getTypeAllocSize(ElemTy);
    for (unsigned I = 0; I < DataArray->getNumElements(); ++I) {
      storeConstant(Region, Offset + I * Stride,
                    DataArray->getElementAsConstant(I));
    }
  } else if (auto *Struct = dyn_cast<ConstantStruct>(C)) {
    const StructLayout *Layout = DL.getStructLayout(Struct->getType());
    for (unsigned I = 0; I < Struct->getNumOperands(); ++I) {
      uint64_t FieldOffset = Offset + Layout->getElementOffset(I);
      storeConstant(Region, FieldOffset, Struct->getOperand(I));
    }
  } else if (auto *AggZero = dyn_cast<ConstantAggregateZero>(C)) {
    uint64_t Len = DL.getTypeStoreSize(AggZero->getType());
    Region.pushOp(MemStore::CreateZero(Offset, Len), DL);
  } else if (C->getType()->isIntOrPtrTy() ||
             C->getType()->isFloatingPointTy()) {
    // Primitive values can be stored directly.
    Region.pushOp(MemStore::CreateStore(Offset, C, EmptyLabels), DL);
  } else if (isa<UndefValue>(C)) {
    // Do nothing - there are no defined values to store.
  } else {
    // All other constants are unsupported for now, and initialize the region
    // with unknown values.
    errs() << "don't know how to store constant " << *C << "\n";
  }
}

std::tuple<Value *, SmallVector<Label, 8>, uint64_t>
Memory::load(Value *Base, uint64_t Offset, Type *T) {
  uint64_t End = Offset + DL.getTypeStoreSize(T);

  auto &Region = getRegion(Base);
  for (auto &Store : make_range(Region.Ops.rbegin(), Region.Ops.rend())) {
    if (Store.overlapsRange(Offset, End, DL)) {
      switch (Store.Kind) {
      case OpStore:
        if (Store.containsRange(Offset, End, DL)) {
          return std::make_tuple(Store.Val, Store.Labels,
                                 Offset - Store.Offset);
        }
        break;
      case OpZero:
        if (Store.containsRange(Offset, End, DL)) {
          return std::make_tuple(Constant::getNullValue(T), Store.Labels, 0);
        }
        break;
      case OpUnknown:
        break;
      }

      // `Store` overlaps this load, but we failed to obtain an appropriate
      // value, so the result is unknown.
      return std::make_tuple(nullptr, EmptyLabels, 0);
    }
  }

  // No store to this region overlaps this value.
  return std::make_tuple(nullptr, EmptyLabels, 0);
}

void Memory::store(Value *Base, uint64_t Offset, Value *V,
                   SmallVector<Label, 8> L) {
  uint64_t End = Offset + DL.getTypeStoreSize(V->getType());

  auto &Region = getRegion(Base);
  // Look for an existing `OpStore` that we can reuse.
  for (auto &Store : make_range(Region.Ops.rbegin(), Region.Ops.rend())) {
    if (Store.overlapsRange(Offset, End, DL)) {
      if (Store.Kind == OpStore && Store.Offset == Offset &&
          Store.getStoreSize(DL) == DL.getTypeStoreSize(V->getType())) {
        Store.Val = V;
        Store.Labels = std::move(L);
        Region.Written = true;
        return;
      }

      // `Store` overlaps the new store but can't be reused.
      break;
    }
  }

  Region.pushOp(MemStore::CreateStore(Offset, V, std::move(L)), DL);
}

void Memory::zero(Value *Base, uint64_t Offset, uint64_t Len) {
  if (Len == 0) {
    return;
  }
  uint64_t End = Offset + Len;

  auto &Region = getRegion(Base);
  if (Region.Ops.size() > 0 && Offset <= Region.MinOffset &&
      End - 1 >= Region.MaxOffset) {
    // We're overwriting all data currently in the region.
    Region.Ops.clear();
  }
  Region.pushOp(MemStore::CreateZero(Offset, Len), DL);
}

void Memory::setUnknown(Value *Base, uint64_t Offset, uint64_t Len) {
  uint64_t End = Offset + Len;

  auto &Region = getRegion(Base);
  if (Region.Ops.size() == 0) {
    // The region is already entirely unknown.
    return;
  } else if (Region.Ops.size() > 0 && Offset <= Region.MinOffset &&
             End - 1 >= Region.MaxOffset) {
    // We're overwriting all data currently in the region.  Afterward, the
    // entire region is unknown, so we don't need to add an explicit op.
    Region.Ops.clear();
    return;
  }
  Region.pushOp(MemStore::CreateUnknown(Offset, Len), DL);
}

void Memory::setLabel(Value *Base, uint64_t Offset, Label L) {
  auto &Region = getRegion(Base);
  // We don't set `Region.Written` because no data is being modified.  Taint
  // labels are handled separately.
  for (auto &Store : make_range(Region.Ops.rbegin(), Region.Ops.rend())) {
    if (Store.contains(Offset, DL)) {
      uint64_t RelOffset = Offset - Store.Offset;
      Store.setLabel(RelOffset, L);
      return;
    }
  }
  // No store overlaps this byte.
  if (L != BOTTOM) {
    // Pushing an `OpUnknown` for this byte doesn't change any existing value.
    MemStore NewStore = MemStore::CreateUnknown(Offset, 1);
    NewStore.setLabel(0, L);
    Region.pushOp(std::move(NewStore), DL);
  }
}

/// A call-stack frame for the interpreter.
struct StackFrame {
  /// The function we're currently in.
  Function &Func;
  /// The basic block we were in before the current one.  Used when handling
  /// phi nodes.
  BasicBlock *PrevBB;
  /// The basic block we're currently in.
  BasicBlock *CurBB;
  /// An iterator pointing to the next instruction to execute.  `CurBB` and
  /// `Iter` together make up the "program counter" for this frame of the
  /// stack.
  BasicBlock::iterator Iter;

  /// Map assigning values to all local registers.  The key is a register in
  /// the original program, which is either a function argument or the result
  /// of an instruction.  The value is the corresponding value in the new
  /// program (`NewFunc`/`NewBB`), which can be either a constant or the result
  /// of a new instruction.
  DenseMap<Value *, Value *> Locals;
  /// For each local (identified by its old value), this gives the labels for
  /// each byte of that local.  If a value is missing here, then it is
  /// untainted.
  DenseMap<Value *, SmallVector<Label, 8>> Labels;

  /// When this frame is performing a call, this is the old `Value` for the
  /// return value of that call.  When the call retuns, a mapping from
  /// `ReturnValue` to the new return value will be added to `Locals`.
  Value *ReturnValue;
  /// When this frame is performing a call, this may be non-null to indicate
  /// that the return value should be cast to a different type after returning.
  /// This is used to handle calls through bitcasted function pointers.
  Type *CastReturnType;
  /// When this frame is performing an `invoke`, this is the block to jump to
  /// if the call throws an exception.
  BasicBlock *UnwindDest;

  /// Create a new stack frame for the given function.  The caller is
  /// responsible for adding argument values to `Locals`.
  StackFrame(Function &Func)
      : Func(Func), PrevBB(nullptr), CurBB(&Func.getEntryBlock()),
        Iter(CurBB->begin()), ReturnValue(nullptr), CastReturnType(nullptr),
        UnwindDest(nullptr) {}

  void addArgument(unsigned I, Value *V, SmallVector<Label, 8> L) {
    Argument *Arg = std::next(Func.arg_begin(), I);
    Locals[Arg] = V;
    Labels[Arg] = std::move(L);
  }

  void enterBlock(BasicBlock *BB);
  /// Enter `BB`, or if `BB` is null, advance to the next instruction.
  ///
  /// This is mostly used for call handling, where some kinds of calls are
  /// terminators with an explicit next block, while others are ordinary
  /// instructions.
  void advance(BasicBlock *BB);

  /// Map an old value to the corresponding new value.  If `OldVal` is a
  /// constant, it is returned unchanged; otherwise, this looks up `OldVal` in
  /// `Locals`.
  Value *mapValue(Value *OldVal);
  SmallVector<Label, 8> const &getLabels(Value *OldVal);
};

/// Information about the calling context, used for unwinding.
struct UnwindContext {
  /// When the callee would return, instead it jumps to this new block and (if
  /// the return type is non-void) adds the new return value to the phi node in
  /// this block.
  BasicBlock *ReturnDest;

  /// When the callee would `resume`, instead it jumps to this new block and
  /// adds the resume value to the phi node in this block.
  BasicBlock *UnwindDest;

  /// List of (old) landingpad instructions of enclosing `invoke` instructions.
  /// All the clauses of these landingpads will be added to landingpads in the
  /// callee.
  SmallVector<LandingPadInst *, 2> LandingPads;

  UnwindContext() : ReturnDest(nullptr), UnwindDest(nullptr), LandingPads() {}
};

/// The complete state of the FlattenInit pass.  This contains the current
/// memory and call stack used by the interpreter, the new function and basic
/// block where instruction will be emitted, and a few other things.
struct State {
  Pass &P;
  DataLayout const &DL;
  Function *NewFunc;
  BasicBlock *NewBB;
  TargetLibraryInfo *TLI;

  DenseMap<Value *, Optional<LinearPtr>> EvalCache;
  Memory Mem;
  std::vector<StackFrame> Stack;

  SimplifyQuery SQ;

  State(Pass &P, Function &OldFunc, StringRef NewName, TargetLibraryInfo *TLI)
      : P(P), DL(OldFunc.getParent()->getDataLayout()), TLI(TLI), SQ(DL, TLI),
        Mem(DL) {
    NewFunc = Function::Create(OldFunc.getFunctionType(), OldFunc.getLinkage(),
                               OldFunc.getAddressSpace(), NewName,
                               OldFunc.getParent());
    NewFunc->copyAttributesFrom(&OldFunc);

    NewBB = BasicBlock::Create(NewFunc->getContext(), "flatinit", NewFunc);

    StackFrame SF(OldFunc);
    for (unsigned I = 0; I < OldFunc.arg_size(); ++I) {
      SF.addArgument(I, std::next(NewFunc->arg_begin(), I), EmptyLabels);
    }
    Stack.emplace_back(std::move(SF));
  }

  void run();
  /// Process the next instruction.  Returns `true` on success, or `false` if
  /// the next instruction can't be processed.
  bool step();
  /// Process a call instruction.  Control resumes at `NormalDest` if the
  /// function returns normally, or at the instruction after the call if
  /// `NormalDest` is nullptr.  For invoke instructions, `UnwindDest` gives the
  /// landing pad (for non-invoke calls, `UnwindDest` is nullptr).
  bool stepCall(CallBase *Call, CallBase *OldCall, BasicBlock *NormalDest,
                BasicBlock *UnwindDest);
  bool stepMemset(CallBase *Call);
  bool stepMemmove(CallBase *Call);
  bool stepMalloc(CallBase *Call, CallBase *OldCall);
  bool stepRealloc(CallBase *Call, CallBase *OldCall);
  bool stepFree(CallBase *Call, CallBase *OldCall);
  Value *stepAlloca(AllocaInst *Alloca);
  bool stepSetLabel(CallBase *Call);

  // Allocate a new `GlobalVariable` containing an array.  This is a helper
  // function for `stepMalloc` and such.
  std::pair<GlobalVariable *, Constant *>
  allocateGlobal(Type *ElemTy, uint64_t Count, uint64_t Align, bool Zero);

  /// Load a single byte from memory.  Returns `nullptr` if the value of the
  /// byte is unknown.
  std::pair<Value *, SmallVector<Label, 8>> memLoadByte(Value *Base,
                                                        uint64_t Offset);
  /// Load a value of type `T` from memory.  Returns `nullptr` if the value is
  /// unknown.
  std::pair<Value *, SmallVector<Label, 8>> memLoad(Value *Base,
                                                    uint64_t Offset, Type *T);
  /// Load a null-terminated string pointed to by `V`.
  std::string memLoadString(Value *V);
  void memCopy(Value *DestBase, uint64_t DestOffset, Value *SrcBase,
               uint64_t SrcOffset, uint64_t Len);
  /// Try to convert the result of `Memory::load` to a value of type `T`.
  /// Returns null if the conversion isn't possible.  This is a helper function
  /// for `memLoad` etc.
  Value *convertLoadResult(Value *V, uint64_t Offset, Type *T);

  /// Unwind the interpreter's call stack, emitting the remaining code for each
  /// function on the stack.
  void unwind();
  void unwindFrame(StackFrame &SF, UnwindContext *PrevUC, UnwindContext *UC);

  /// Update global initializers to match the contents of `Mem`.
  ///
  /// Warning: this should be assumed to invalidate ALL stored `Value*`s,
  /// including `StackFrame::Locals` and the values in `Mem`.  This is because
  /// `updateMemory` deletes most `GlobalVariable`s (replacing them with new
  /// ones), but only updates references inside LLVM objects (e.g. a `Constant`
  /// that includes a pointer to the deleted `GlobalVariable`).
  void updateMemory();

  /// Try to evaluate `V` into a base (pointer) and offset form.
  Optional<std::pair<Value *, uint64_t>> evalBaseOffset(Value *V);
  LinearPtr *evalPtr(Value *V);
  Optional<LinearPtr> evalPtrImpl(Value *V);
  Optional<LinearPtr> evalPtrConstant(Constant *C);
  Optional<LinearPtr> evalPtrInstruction(Instruction *Inst);
  Optional<LinearPtr> evalPtrOpcode(unsigned Opcode, User *U);
  Optional<LinearPtr> evalPtrGEP(User *U);

  /// General-purpose instruction simplification and constant folding.  This
  /// may return `Inst` itself, some existing new instruction, or a constant.
  Value *foldInst(Instruction *Inst);
  /// Apply `foldInst` to `Inst`, emit `Inst` into `NewBB` if needed, and
  /// return the resulting value.  `Inst` will be deleted automatically if this
  /// is appropriate (so `Inst` should not be used afterward - but the returned
  /// `Value*` is guaranteed to evaluate to the same value as `Inst`).  This
  /// method should be used when synthesizing a new instruction from scratch.
  Value *foldAndEmitInst(Instruction *Inst);
  // Compute the label for a folded instruction.
  SmallVector<Label, 8> labelInst(StackFrame *SF, Instruction *Inst);

  // Check that the label is BOTTOM or error out.
  void checkBottom(SmallVector<Label, 8> L);

  /// Constant fold, plus some extra cases.
  Constant *constantFoldExtra(Constant *C);
  /// Wrapper around llvm::ConstantFoldConstant.
  Constant *constantFoldConstant(Constant *C);
  Constant *constantFoldAlignmentCheckAnd(Constant *C);
  Constant *constantFoldAlignmentCheckURem(Constant *C);
  Constant *constantFoldAlignmentCheckPtr(Constant *C, LinearPtr *LP,
                                          uint64_t Align, uint64_t Mask);

  /// Constant fold, plus some extra cases.  Returns nullptr if it was unable
  /// to reduce `Inst` to a constant.
  Constant *constantFoldInstructionExtra(Instruction *Inst);
  Constant *constantFoldNullCheckInst(Instruction *Inst);
  Constant *constantFoldPointerCompare(Instruction *Inst);
};

// Flattening

void State::run() {
  while (step()) {
    // No-op
  }
  errs() << "\n\n\n ===== UNWINDING =====\n\n\n";
  while (!NewBB->empty()) {
    NewBB->begin()->eraseFromParent();
  }
  if (Stack.size() > 0) {
    unwind();
  }
  errs() << "\n\n\n ===== UPDATING MEMORY =====\n\n\n";
  updateMemory();
}

bool State::step() {
  if (Stack.size() == 0) {
    return false;
  }
  StackFrame &SF = Stack.back();
  Instruction *OldInst = &*SF.Iter;

  // Phi nodes need special handling: they must be executed simultaneously, not
  // sequentially, on entry to the block.
  if (isa<PHINode>(OldInst)) {
    assert(SF.PrevBB != nullptr);

    // Compute all the new values first, so that all updates to `SF.Locals`
    // happen at once.
    SmallVector<std::tuple<PHINode *, Value *, SmallVector<Label, 8>>, 4>
        NewValues;
    while (auto *PHI = dyn_cast<PHINode>(&*SF.Iter)) {
      Value *OldVal = PHI->getIncomingValueForBlock(SF.PrevBB);
      Value *NewVal = SF.mapValue(OldVal);
      SmallVector<Label, 8> OldLabel = SF.getLabels(OldVal);
      NewValues.push_back(std::make_tuple(PHI, NewVal, OldLabel));
      ++SF.Iter;
    }

    for (auto &Pair : NewValues) {
      SF.Locals[std::get<0>(Pair)] = std::get<1>(Pair);
      SF.Labels[std::get<0>(Pair)] = std::get<2>(Pair);
    }

    return true;
  }

  Instruction *Inst(OldInst->clone());
  Inst->setName(OldInst->getName());
  for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
    Value *OldVal = Inst->getOperand(I);
    if (!isa<BasicBlock>(OldVal)) {
      Value *NewVal = SF.mapValue(OldVal);
      Inst->setOperand(I, NewVal);
    }
  }

  // Try simplification and/or constant folding.

  Value *Folded = foldInst(Inst);
  if (Folded != Inst) {
    SF.Locals[OldInst] = Folded;
    SmallVector<Label, 8> FoldedLabel = labelInst(&SF, Inst);
    SF.Labels[OldInst] = FoldedLabel;
    Inst->deleteValue();
    ++SF.Iter;
    return true;
  }

  // Instructions with special handling.  If these special cases fail, we fall
  // through and (usually) emit the instruction unchanged.

  // Try to handle function calls.  This can fail if the callee is unknown or
  // not defined, in which case we pass it through as an unknown instruction.
  if (auto *Call = dyn_cast<CallInst>(Inst)) {
    if (stepCall(Call, cast<CallInst>(OldInst), nullptr, nullptr)) {
      // Checks on target.
      auto *Callee = Call->getCalledOperand();
      auto const &CalleeL = SF.getLabels(Callee);
      checkBottom(CalleeL);

      return true;
    }
  }
  if (auto *Invoke = dyn_cast<InvokeInst>(Inst)) {
    if (stepCall(Invoke, cast<InvokeInst>(OldInst), Invoke->getNormalDest(),
                 Invoke->getUnwindDest())) {
      // Checks on target.
      auto *Callee = Invoke->getCalledOperand();
      auto const &CalleeL = SF.getLabels(Callee);
      checkBottom(CalleeL);

      return true;
    }
  }

  if (auto *Return = dyn_cast<ReturnInst>(Inst)) {
    if (Stack.size() == 1) {
      // Returning from the top-level function should just be passed through.
      return false;
    }

    // `Return` is the new instruction (with operands already mapped), so it's
    // not invalidated when we deallocate `SF`.
    Value *RetVal = Return->getReturnValue();
    auto const &RetL = SF.getLabels(RetVal);

    Stack.pop_back();
    StackFrame &PrevSF = Stack.back();
    if (PrevSF.ReturnValue != nullptr && RetVal != nullptr) {
      if (PrevSF.CastReturnType != nullptr) {
        RetVal = foldAndEmitInst(CastInst::CreateBitOrPointerCast(
            RetVal, PrevSF.CastReturnType, "returncast"));
      }
      // Propagate label for returned value.
      PrevSF.Labels[PrevSF.ReturnValue] = RetL;
      PrevSF.Locals[PrevSF.ReturnValue] = RetVal;
    }
    PrevSF.ReturnValue = nullptr;
    PrevSF.CastReturnType = nullptr;
    PrevSF.UnwindDest = nullptr;

    Inst->deleteValue();
    // `PrevSF.Iter` was already updated when the call was processed.
    return true;
  }

  if (auto *Branch = dyn_cast<BranchInst>(Inst)) {
    if (Branch->isUnconditional()) {
      // Taint analysis checks on target are unnecessary since the successor is
      // a compile-time constant.
      auto *Succ = Branch->getSuccessor(0);

      SF.enterBlock(Succ);
      Inst->deleteValue();
      return true;
    } else {
      if (auto *ConstCond = dyn_cast<Constant>(Branch->getCondition())) {
        // Checks on conditional.
        auto const &CondL = SF.getLabels(ConstCond);
        checkBottom(CondL);
        // Taint analysis checks on target are unnecessary since the successor
        // is a compile-time constant.
        auto *Succ = ConstCond->isOneValue() ? Branch->getSuccessor(0)
                                             : Branch->getSuccessor(1);

        SF.enterBlock(Succ);
        Inst->deleteValue();
        return true;
      }
    }
  }
  if (auto *Switch = dyn_cast<SwitchInst>(Inst)) {
    if (auto *ConstCond = dyn_cast<ConstantInt>(Switch->getCondition())) {
      // Checks on target and conditional.
      auto const &CondL = SF.getLabels(ConstCond);
      checkBottom(CondL);
      // Taint analysis checks on target are unnecessary since the successor is
      // a compile-time constant.
      auto *Succ = Switch->findCaseValue(ConstCond)->getCaseSuccessor();

      SF.enterBlock(Succ);
      Inst->deleteValue();
      return true;
    }
  }

  if (auto *Load = dyn_cast<LoadInst>(Inst)) {
    auto *RawPtr = Load->getPointerOperand();
    if (auto Ptr = evalBaseOffset(RawPtr)) {
      // Checks on addr.
      auto const &AddrL = SF.getLabels(RawPtr);
      checkBottom(AddrL);

      Value *V;
      SmallVector<Label, 8> L;
      std::tie(V, L) = memLoad(Ptr->first, Ptr->second, Load->getType());
      if (V != nullptr) {
        SF.Locals[OldInst] = V;
        if (L.size() > 0) {
          errs() << "load ";
          OldInst->printAsOperand(errs());
          errs() << " from ";
          Ptr->first->printAsOperand(errs());
          errs() << " +" << Ptr->second << ": got labels ";
          for (unsigned I = 0; I < L.size(); ++I) {
            if (I > 0) {
              errs() << ", ";
            }
            errs() << (int)L[I];
          }
          errs() << "\n";
          SF.Labels[OldInst] = L;
        }
        Inst->deleteValue();
        ++SF.Iter;
        return true;
      }
    } else {
      errs() << "failed to evaluate to a pointer expression: "
             << *Load->getPointerOperand() << "\n";
    }
  }

  // Allocation instructions.
  if (auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
    if (Value *V = stepAlloca(Alloca)) {
      SF.Locals[OldInst] = V;
      Inst->deleteValue();
      ++SF.Iter;
      return true;
    }
  }

  // We can't pass through unknown terminators, because we don't know what to
  // execute next.
  if (Inst->isTerminator()) {
    errs() << "unwinding due to (old) " << *OldInst << "\n";
    errs() << "unwinding due to (new) " << *Inst << "\n";
    Inst->deleteValue();
    return false;
  }

  // Instructions that pass through, but with some special effect.
  if (auto *Store = dyn_cast<StoreInst>(Inst)) {
    // We could actually handle some of these cases (e.g. stores of
    // non-constant values) without bailing out, but we'd have to call
    // `updateMemory()` before handling the instruction, which would require
    // improving `updateMemory` so it doesn't invalidate `Locals` and other
    // tables that store `Value`s.
    if (!isa<Constant>(Store->getValueOperand())) {
      errs() << "storing a non-constant value in " << *Store << "\n";
      errs() << "unwinding due to (old) " << *OldInst << "\n";
      errs() << "unwinding due to (new) " << *Inst << "\n";
      Inst->deleteValue();
      return false;
    }
    auto *RawPtr = Store->getPointerOperand();
    if (auto Ptr = evalBaseOffset(RawPtr)) {
      // Checks on addr.
      auto const &AddrL = SF.getLabels(RawPtr);
      checkBottom(AddrL);

      Value *V = Store->getValueOperand();
      Value *OldV = cast<StoreInst>(OldInst)->getValueOperand();
      auto const &L = SF.getLabels(OldV);
      if (L.size() > 0) {
        errs() << "store to ";
        Ptr->first->printAsOperand(errs());
        errs() << " +" << Ptr->second << " with labels ";
        for (unsigned I = 0; I < L.size(); ++I) {
          if (I > 0) {
            errs() << ", ";
          }
          errs() << (int)L[I];
        }
        errs() << "\n";
      }
      Mem.store(Ptr->first, Ptr->second, V, L);
    } else {
      errs() << "clear mem: unknown store dest in " << *Store << "\n";
      // Mem.clear();
      errs() << "unwinding due to (old) " << *OldInst << "\n";
      errs() << "unwinding due to (new) " << *Inst << "\n";
      Inst->deleteValue();
      return false;
    }
  } else {
    errs() << "unwinding due to (old) " << *OldInst << "\n";
    errs() << "unwinding due to (new) " << *Inst << "\n";
    Inst->deleteValue();
    return false;
  }

  SF.Locals[OldInst] = Inst;
  NewBB->getInstList().push_back(Inst);
  ++SF.Iter;
  return true;
}

Value *State::convertLoadResult(Value *V, uint64_t Offset, Type *T) {
  Type *SrcTy = V->getType();
  if (SrcTy == T) {
    return V;
  }

  // Check if we can just do a simple cast.
  if (CastInst::isBitOrNoopPointerCastable(SrcTy, T, DL)) {
    return foldAndEmitInst(CastInst::CreateBitOrPointerCast(V, T, "loadcast"));
  }

  // Complex case: extracting a byte from a larger value.
  if (auto *IntTy = dyn_cast<IntegerType>(T)) {
    if (SrcTy->isIntOrPtrTy()) {
      Value *Src = V;
      if (auto *SrcPtrTy = dyn_cast<PointerType>(SrcTy)) {
        SrcTy = DL.getIntPtrType(SrcTy);
        Src = foldAndEmitInst(new PtrToIntInst(Src, SrcTy, "loadcast"));
      }
      if (Offset > 0) {
        auto *ShiftAmount = ConstantInt::get(SrcTy, 8 * Offset);
        Src = foldAndEmitInst(BinaryOperator::Create(Instruction::LShr, Src,
                                                     ShiftAmount, "loadshift"));
      }
      return foldAndEmitInst(
          CastInst::Create(Instruction::Trunc, Src, T, "loadtrunc"));
    }
  }

  errs() << "load failed: can't extract " << *T << " from offset " << Offset
         << " of " << *V << "\n";
  return nullptr;
}

void StackFrame::enterBlock(BasicBlock *BB) {
  PrevBB = CurBB;
  CurBB = BB;
  Iter = BB->begin();
}

void StackFrame::advance(BasicBlock *BB) {
  if (BB == nullptr) {
    ++Iter;
  } else {
    enterBlock(BB);
  }
}

Value *StackFrame::mapValue(Value *OldVal) {
  if (isa<Constant>(OldVal)) {
    return OldVal;
  }

  auto It = Locals.find(OldVal);
  if (It == Locals.end()) {
    errs() << "error: no local mapping for value " << *OldVal << "\n";
    assert(0 && "no local mapping for value");
  }
  return It->second;
}

SmallVector<Label, 8> const &StackFrame::getLabels(Value *OldVal) {
  auto It = Labels.find(OldVal);
  if (It == Labels.end()) {
    return EmptyLabels;
  }
  return It->second;
}

Function *getCallee(Value *V) {
  if (auto *Func = dyn_cast<Function>(V)) {
    return Func;
  } else if (auto *Expr = dyn_cast<ConstantExpr>(V)) {
    if (Expr->getOpcode() == Instruction::BitCast) {
      return getCallee(Expr->getOperand(0));
    }
  } else if (auto *Cast = dyn_cast<CastInst>(V)) {
    if (Cast->getOpcode() == Instruction::BitCast) {
      return getCallee(Cast->getOperand(0));
    }
  }
  return nullptr;
}

bool State::stepCall(CallBase *Call, CallBase *OldCall, BasicBlock *NormalDest,
                     BasicBlock *UnwindDest) {
  // Never step into malloc or free functions.  If the special handler fails to
  // handle it, bail out.
  if (isReallocLikeFn(Call, TLI)) {
    if (stepRealloc(Call, OldCall)) {
      Stack.back().advance(NormalDest);
      return true;
    }
    return false;
  }
  if (isAllocationFn(Call, TLI)) {
    if (stepMalloc(Call, OldCall)) {
      Stack.back().advance(NormalDest);
      return true;
    }
    return false;
  }
  if (isFreeCall(Call, TLI)) {
    if (stepFree(Call, OldCall)) {
      Stack.back().advance(NormalDest);
      return true;
    }
    return false;
  }

  // Get the callee and the signature used for the call (which may differ from
  // the callee's actual signature, if the callee has been bitcasted).
  Function *Callee = getCallee(Call->getCalledOperand());
  FunctionType *CallSig = Call->getFunctionType();

  // Unknown/indirect callee that cannot be resolved to a Function.
  // Fail conservatively so the caller can unwind instead of dereferencing null.
  if (Callee == nullptr) {
    return false;
  }

  if (Callee->isIntrinsic() && CallSig == Callee->getFunctionType()) {
    // Each of `stepFoo` functions called here (e.g. `stepMemset`) is expected
    // to dispose of `Call` appropriately (either deleting it or adding it to
    // `NewBB`) if it returns `true`.
    switch (Callee->getIntrinsicID()) {
    case Intrinsic::memset:
      if (stepMemset(Call)) {
        Stack.back().advance(NormalDest);
        return true;
      }
      break;
    case Intrinsic::memcpy:
    case Intrinsic::memmove:
      if (stepMemmove(Call)) {
        Stack.back().advance(NormalDest);
        return true;
      }
      break;
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
      Stack.back().advance(NormalDest);
      NewBB->getInstList().push_back(Call);
      return true;
    }
  }

  // Handle MicroRAM intrinsics.
  if (Callee->getName() == "__cc_trace") {
    errs() << "[TRACE] " << memLoadString(Call->getArgOperand(0)) << "\n";
    Stack.back().advance(NormalDest);
    Call->deleteValue();
    return true;
  }
  if (Callee->getName() == "__cc_trace_exec") {
    errs() << "[FUNC] " << memLoadString(Call->getArgOperand(0)) << "(";
    unsigned LastArg = 4;
    Type *WordTy = IntegerType::get(NewFunc->getContext(), 64);
    Value *Zero = ConstantInt::get(WordTy, 0);
    while (LastArg >= 1) {
      if (Call->getArgOperand(LastArg) == Zero) {
        --LastArg;
      } else {
        break;
      }
    }
    for (unsigned I = 1; I <= LastArg; ++I) {
      if (I > 1) {
        errs() << ", ";
      }
      Value *V = Call->getArgOperand(I);
      if (auto *Int = dyn_cast<ConstantInt>(V)) {
        errs() << format_hex(Int->getZExtValue(), 0);
        continue;
      }
      if (auto *PtrToInt = dyn_cast<ConstantExpr>(V)) {
        if (PtrToInt->getOpcode() == Instruction::PtrToInt) {
          V = PtrToInt->getOperand(0);
        }
      }
      if (auto *GV = dyn_cast<GlobalObject>(V)) {
        errs() << "@" << GV->getName();
        continue;
      }
      errs() << *V;
    }
    errs() << ")\n";
    Stack.back().advance(NormalDest);
    Call->deleteValue();
    return true;
  }
  if (Callee->getName() == "noniSetLabelU8") {
    if (stepSetLabel(Call)) {
      Stack.back().advance(NormalDest);
      return true;
    }
    return false;
  }
  if (Callee->getName() == "noniSinkU8") {
    // A no-op, for our purposes.
    Stack.back().advance(NormalDest);
    Call->deleteValue();
    return true;
  }

  if (Callee->isDeclaration()) {
    // Function body is not available.
    return false;
  }

  if (Callee->getFunctionType() != CallSig) {
    // The callee has been bitcasted to another signature.  Check for
    // compatibility.
    FunctionType *DefSig = Callee->getFunctionType();
    if (Call->arg_size() < DefSig->getNumParams()) {
      // Calling with too few arguments is not supported.
      return false;
    }
    for (unsigned I = 0; I < DefSig->getNumParams(); ++I) {
      Type *CallTy = Call->getArgOperand(I)->getType();
      Type *DefTy = DefSig->getParamType(I);
      if (CallTy != DefTy &&
          !CastInst::isBitOrNoopPointerCastable(CallTy, DefTy, DL)) {
        // Argument types are not convertible.
        return false;
      }
    }

    Type *CallReturnTy = Call->getType();
    Type *DefReturnTy = DefSig->getReturnType();
    if (CallReturnTy != DefReturnTy && !CallReturnTy->isVoidTy() &&
        !CastInst::isBitOrNoopPointerCastable(DefReturnTy, CallReturnTy, DL)) {
      // Return type is non-void and not convertible.
      return false;
    }
  }

  // Advance the current frame past the call.
  StackFrame &SF = Stack.back();
  SF.advance(NormalDest);

  SF.ReturnValue = OldCall;
  if (Callee->getFunctionType()->getReturnType() != Call->getType()) {
    SF.CastReturnType = Call->getType();
  }
  SF.UnwindDest = UnwindDest;

  // Push a new frame to run the callee.
  StackFrame NewSF(*Callee);
  for (unsigned I = 0; I < Callee->getFunctionType()->getNumParams(); ++I) {
    Value *V = Call->getArgOperand(I);
    Type *ExpectTy = Callee->getFunctionType()->getParamType(I);
    if (V->getType() != ExpectTy) {
      V = foldAndEmitInst(
          CastInst::CreateBitOrPointerCast(V, ExpectTy, "callcast"));
    }
    NewSF.addArgument(I, V, SF.getLabels(OldCall->getArgOperand(I)));
  }
  Stack.emplace_back(std::move(NewSF));

  Call->deleteValue();
  return true;
}

bool State::stepMemset(CallBase *Call) {
  auto DestPtr = evalBaseOffset(Call->getOperand(0));
  if (!DestPtr) {
    return false;
  }

  auto *ValConst = dyn_cast<Constant>(Call->getOperand(1));
  if (ValConst == nullptr) {
    return false;
  }

  auto *LenConst = dyn_cast<ConstantInt>(Call->getOperand(2));
  if (LenConst == nullptr) {
    return false;
  }
  uint64_t Len = LenConst->getZExtValue();

  // We ignore operand 3, the `isvolatile` flag.

  if (ValConst->isZeroValue()) {
    Mem.zero(DestPtr->first, DestPtr->second, Len);
  } else {
    Type *ByteTy = IntegerType::get(NewFunc->getContext(), 8);
    auto *ValByte = ConstantExpr::getTrunc(ValConst, ByteTy);
    for (unsigned I = 0; I < Len; ++I) {
      Mem.store(DestPtr->first, DestPtr->second + I, ValByte, EmptyLabels);
    }
  }

  NewBB->getInstList().push_back(Call);
  return true;
}

bool State::stepMemmove(CallBase *Call) {
  auto DestPtr = evalBaseOffset(Call->getOperand(0));
  if (!DestPtr) {
    return false;
  }

  auto SrcPtr = evalBaseOffset(Call->getOperand(1));
  if (!SrcPtr) {
    return false;
  }

  auto *LenConst = dyn_cast<ConstantInt>(Call->getOperand(2));
  if (LenConst == nullptr) {
    return false;
  }
  uint64_t Len = LenConst->getZExtValue();

  // We ignore operand 3, the `isvolatile` flag.

  if (SrcPtr->first == DestPtr->first && SrcPtr->second == DestPtr->second) {
    // Source and dest pointers are the same - this is a no-op.
    Call->deleteValue();
    return true;
  }

  memCopy(DestPtr->first, DestPtr->second, SrcPtr->first, SrcPtr->second, Len);

  NewBB->getInstList().push_back(Call);
  return true;
}

std::pair<GlobalVariable *, Constant *>
State::allocateGlobal(Type *ElemTy, uint64_t Count, uint64_t Align, bool Zero) {
  Type *Ty = ArrayType::get(ElemTy, Count);
  Constant *Init;
  if (Zero) {
    Init = ConstantAggregateZero::get(Ty);
  } else {
    Init = UndefValue::get(Ty);
  }
  GlobalVariable *GV =
      new GlobalVariable(*NewFunc->getParent(), Ty,
                         false, // not constant
                         GlobalValue::InternalLinkage, Init, "alloc");
  GV->setAlignment(MaybeAlign(Align));
  Constant *GEPIdxs[2] = {
      ConstantInt::get(IntegerType::get(NewFunc->getContext(), 64), 0),
      ConstantInt::get(IntegerType::get(NewFunc->getContext(), 32), 0),
  };
  Constant *Ptr = ConstantExpr::getInBoundsGetElementPtr(Ty, GV, GEPIdxs);
  return std::make_pair(GV, Ptr);
}

bool State::stepMalloc(CallBase *Call, CallBase *OldCall) {
  Function *Callee = Call->getCalledFunction();
  if (Callee == nullptr) {
    return false;
  }
  LibFunc LF;
  if (!TLI->getLibFunc(*Callee, LF)) {
    return false;
  }

  // The size of the allocated region.  This is always set.
  Value *SizeV;
  // If set, the size of the allocated region is the product of `Size` and
  // `Size2`, instead of just `Size`.
  Value *Size2V = nullptr;
  // If set, the allocated region is aligned to a multiple of this amount.
  Value *AlignV = nullptr;
  // If set, the allocated region is zeroed.
  bool Zero = false;
  // Out-pointer argument.  If set, the function stores the allocated pointer
  // here and returns zero, instead of returning the pointer directly.
  Value *OutPtrV = nullptr;

  switch (LF) {
  case LibFunc_malloc:
    SizeV = Call->getOperand(0);
    break;
  case LibFunc_calloc:
    SizeV = Call->getOperand(0);
    Size2V = Call->getOperand(1);
    Zero = true;
    break;
  case LibFunc_posix_memalign:
    OutPtrV = Call->getOperand(0);
    AlignV = Call->getOperand(1);
    SizeV = Call->getOperand(2);
    break;
  default:
    errs() << "failed to handle malloc: unsupported callee in " << *Call
           << "\n";
    return false;
  }

  // Convert the arguments.
  uint64_t Size;
  if (auto *Int = dyn_cast<ConstantInt>(SizeV)) {
    Size = Int->getZExtValue();
  } else {
    errs() << "malloc failed: non-constant size, in " << *Call << "\n";
    return false;
  }

  uint64_t Size2 = 1;
  if (Size2V != nullptr) {
    if (auto *Int = dyn_cast<ConstantInt>(Size2V)) {
      Size2 = Int->getZExtValue();
    } else {
      errs() << "malloc failed: non-constant size2, in " << *Call << "\n";
      return false;
    }
  }

  // If `Size2` is set, set `Size = Size * Size2`, but check for overflow.
  if (Size2 != 1) {
    uint64_t Product;
    if (!__builtin_mul_overflow(Size, Size2, &Product)) {
      errs() << "malloc failed: size product overflowed, in " << *Call << "\n";
      return false;
    }
    Size = Product;
  }

  uint64_t Align;
  if (AlignV != nullptr) {
    if (auto *Int = dyn_cast<ConstantInt>(AlignV)) {
      Align = Int->getZExtValue();
    } else {
      errs() << "malloc failed: non-constant align, in " << *Call << "\n";
      return false;
    }
  } else {
    Align = 8;
  }

  Optional<std::pair<Value *, uint64_t>> OutPtr = None;
  if (OutPtrV != nullptr) {
    OutPtr = evalBaseOffset(OutPtrV);
    if (!OutPtr) {
      errs() << "malloc failed: failed to evaluate out ptr, in " << *Call
             << "\n";
      return false;
    }
  }

  // Create the allocation.
  auto Global = allocateGlobal(IntegerType::get(NewFunc->getContext(), 8), Size,
                               Align, Zero);
  GlobalVariable *GV = Global.first;
  Value *Ptr = Global.second;

  Value *ReturnValue;
  if (OutPtr) {
    Mem.store(OutPtr->first, OutPtr->second, Ptr, EmptyLabels);
    ReturnValue = ConstantInt::get(Call->getType(), 0);
  } else {
    ReturnValue = Ptr;
  }

  // Add the allocation to memory.  Note `GV` is the base, not `Ptr` itself.
  EvalCache[GV] = LinearPtr(GV);

  Stack.back().Locals[OldCall] = ReturnValue;
  Call->deleteValue();
  return true;
}

bool State::stepRealloc(CallBase *Call, CallBase *OldCall) {
  Function *Callee = Call->getCalledFunction();
  if (Callee == nullptr) {
    return false;
  }
  LibFunc LF;
  if (!TLI->getLibFunc(*Callee, LF)) {
    return false;
  }

  Value *OldPtrV;
  Value *SizeV;

  switch (LF) {
  case LibFunc_realloc:
    OldPtrV = Call->getOperand(0);
    SizeV = Call->getOperand(1);
    break;
  default:
    errs() << "failed to handle realloc: unsupported callee in " << *Call
           << "\n";
    return false;
  }

  // Convert the arguments.
  auto OldPtr = evalBaseOffset(OldPtrV);
  if (!OldPtr) {
    errs() << "realloc failed: failed to evaluate ptr, in " << *Call << "\n";
    return false;
  }

  uint64_t Size;
  if (auto *Int = dyn_cast<ConstantInt>(SizeV)) {
    Size = Int->getZExtValue();
  } else {
    errs() << "realloc failed: non-constant size, in " << *Call << "\n";
    return false;
  }

  uint64_t MemcpySize = Size;
  if (OldPtr->second != 0) {
    errs() << "realloc failed: old pointer has nonzero offset, in " << *Call
           << "\n";
    return false;
  } else if (OldPtr->first == nullptr) {
    // If the old pointer is NULL, there's nothing to copy.
    MemcpySize = 0;
  } else if (auto *OldGV = dyn_cast<GlobalVariable>(OldPtr->first)) {
    uint64_t GVSize = DL.getTypeAllocSize(OldGV->getValueType());
    if (GVSize < MemcpySize) {
      MemcpySize = GVSize;
    }
  } else {
    errs() << "realloc failed: couldn't get size of old pointer, in " << *Call
           << "\n";
    errs() << "  old pointer is " << *OldPtr->first << "\n";
    return false;
  }

  uint64_t Align = 8;
  bool Zero = false;

  // Create the allocation.
  auto Global = allocateGlobal(IntegerType::get(NewFunc->getContext(), 8), Size,
                               Align, Zero);
  GlobalVariable *GV = Global.first;
  Value *Ptr = Global.second;

  // Add the allocation to memory, and copy over the old contents.  Note `GV`
  // is the base, not `Ptr` itself.
  EvalCache[GV] = LinearPtr(GV);
  if (MemcpySize != 0) {
    memCopy(GV, 0, OldPtr->first, 0, MemcpySize);

    // Emit a call to `memcpy` to initialize the new global from the old.
    Type *SizeTy = IntegerType::get(NewFunc->getContext(), 64);
    Type *MemcpyTys[3] = {Ptr->getType(), OldPtrV->getType(), SizeTy};
    Function *MemcpyFunc = Intrinsic::getDeclaration(
        NewFunc->getParent(), Intrinsic::memcpy, MemcpyTys);
    Value *MemcpyArgs[4] = {Ptr, OldPtrV, ConstantInt::get(SizeTy, MemcpySize),
                            ConstantInt::getFalse(NewFunc->getContext())};
    CallInst::Create(MemcpyFunc->getFunctionType(), MemcpyFunc, MemcpyArgs, "",
                     NewBB);
  }

  Stack.back().Locals[OldCall] = Ptr;
  Call->deleteValue();
  return true;
}

bool State::stepFree(CallBase *Call, CallBase *OldCall) {
  Function *Callee = Call->getCalledFunction();
  if (Callee == nullptr) {
    return false;
  }
  LibFunc LF;
  if (!TLI->getLibFunc(*Callee, LF)) {
    return false;
  }

  switch (LF) {
  case LibFunc_free:
    break;
  default:
    errs() << "failed to handle free: unsupported callee in " << *Call << "\n";
    return false;
  }

  // free(ptr) is a no-op in this model.
  Call->deleteValue();
  return true;
}

Value *State::stepAlloca(AllocaInst *Alloca) {
  uint64_t Count;
  if (auto *Int = dyn_cast<ConstantInt>(Alloca->getArraySize())) {
    Count = Int->getZExtValue();
  } else {
    errs() << "alloca failed: non-constant size, in " << *Alloca << "\n";
    return nullptr;
  }

  uint64_t Align = 8;
  bool Zero = false;

  auto Global = allocateGlobal(Alloca->getAllocatedType(), Count, Align, Zero);
  GlobalVariable *GV = Global.first;
  Value *Ptr = Global.second;

  EvalCache[GV] = LinearPtr(GV);
  return Ptr;
}

bool State::stepSetLabel(CallBase *Call) {
  auto Ptr = evalBaseOffset(Call->getArgOperand(0));
  if (!Ptr) {
    return false;
  }

  auto *LabelConst = dyn_cast<ConstantInt>(Call->getArgOperand(1));
  if (LabelConst == nullptr) {
    return false;
  }
  Label L = LabelConst->getZExtValue() & LABEL_MASK;

  errs() << "set label of ";
  Ptr->first->printAsOperand(errs());
  errs() << " +" << Ptr->second << " to " << (int)L << "\n";

  Mem.setLabel(Ptr->first, Ptr->second, L);
  Call->deleteValue();
  return true;
}

std::pair<Value *, SmallVector<Label, 8>> State::memLoadByte(Value *Base,
                                                             uint64_t Offset) {
  Type *ByteTy = IntegerType::get(NewFunc->getContext(), 8);
  Value *LoadVal;
  SmallVector<Label, 8> LoadLabels;
  uint64_t LoadOffset;
  std::tie(LoadVal, LoadLabels, LoadOffset) = Mem.load(Base, Offset, ByteTy);
  if (LoadVal == nullptr) {
    return std::make_pair(nullptr, EmptyLabels);
  }
  return std::make_pair(convertLoadResult(LoadVal, LoadOffset, ByteTy),
                        LoadLabels);
}

void dumpMemRegion(MemRegion const &Region) {
  for (auto &Store : Region.Ops) {
    errs() << "  +" << Store.Offset << ": ";
    if (Store.Kind == OpStore) {
      errs() << "store ";
      Store.Val->printAsOperand(errs());
      errs() << "\n";
    } else {
      errs() << "fill " << Store.Len << " bytes with kind " << Store.Kind
             << "\n";
    }
  }
}

std::pair<Value *, SmallVector<Label, 8>>
State::memLoad(Value *Base, uint64_t Offset, Type *T) {
  Value *LoadVal;
  SmallVector<Label, 8> LoadLabels;
  uint64_t LoadOffset;
  std::tie(LoadVal, LoadLabels, LoadOffset) = Mem.load(Base, Offset, T);
  if (LoadVal != nullptr) {
    if (auto *V = convertLoadResult(LoadVal, LoadOffset, T)) {
      return std::make_pair(V, LoadLabels);
    }
  }

  auto *IntTy = dyn_cast<IntegerType>(T);
  if (IntTy == nullptr) {
    errs() << "memLoad failed: can't do bytewise load of " << *T << " from ";
    Base->printAsOperand(errs());
    errs() << " +" << Offset << "\n";

    errs() << "dump region ";
    Base->printAsOperand(errs());
    errs() << ":\n";
    dumpMemRegion(Mem.getRegion(Base));

    return std::make_pair(nullptr, EmptyLabels);
  }

  uint64_t Size = DL.getTypeStoreSize(T);
  SmallVector<Value *, 8> Bytes;
  SmallVector<Label, 8> Labels;
  for (uint64_t I = 0; I < Size; ++I) {
    Value *Byte;
    SmallVector<Label, 8> ByteLabels;
    std::tie(Byte, ByteLabels) = memLoadByte(Base, Offset + I);
    if (Byte == nullptr) {
      errs() << "memLoad failed: can't get byte at ";
      Base->printAsOperand(errs());
      errs() << " +" << (Offset + I) << "\n";

      errs() << "dump region ";
      Base->printAsOperand(errs());
      errs() << ":\n";
      dumpMemRegion(Mem.getRegion(Base));

      return std::make_pair(nullptr, EmptyLabels);
    }
    Bytes.push_back(Byte);
    if (ByteLabels.size() > 0) {
      if (Labels.size() <= I) {
        Labels.resize(I + 1, BOTTOM);
      }
      Labels[I] = ByteLabels[0];
    }
  }

  Value *V = ConstantInt::get(IntTy, 0);
  for (uint64_t I = 0; I < Size; ++I) {
    Value *Byte = Bytes[I];
    Byte = foldAndEmitInst(
        CastInst::Create(Instruction::ZExt, Byte, IntTy, "loadext"));
    if (I > 0) {
      auto *ShiftAmount = ConstantInt::get(IntTy, 8 * I);
      Byte = foldAndEmitInst(BinaryOperator::Create(Instruction::Shl, Byte,
                                                    ShiftAmount, "loadshift"));
    }
    V = foldAndEmitInst(
        BinaryOperator::Create(Instruction::Or, V, Byte, "loadconcat"));
  }
  return std::make_pair(V, Labels);
}

std::string State::memLoadString(Value *V) {
  std::string S;
  raw_string_ostream Out(S);

  auto Ptr = evalBaseOffset(V);
  if (!Ptr) {
    return "???";
  }

  for (unsigned I = 0;; ++I) {
    Value *Byte = memLoadByte(Ptr->first, Ptr->second + I).first;
    if (Byte == nullptr) {
      Out << "???";
      break;
    }
    auto *Int = dyn_cast<ConstantInt>(Byte);
    if (Int == nullptr) {
      Out << "???";
      break;
    }
    uint64_t Value = Int->getZExtValue();
    if (Value == 0) {
      break;
    }
    Out << (char)Value;
  }

  return Out.str();
}

void State::memCopy(Value *DestBase, uint64_t DestOffset, Value *SrcBase,
                    uint64_t SrcOffset, uint64_t Len) {
  if (SrcBase == DestBase) {
    // Allocate an extra buffer for temporary storage.
    auto TempBase = allocateGlobal(IntegerType::get(NewFunc->getContext(), 8),
                                   Len, 1, false);
    memCopy(TempBase.first, 0, SrcBase, SrcOffset, Len);
    memCopy(DestBase, DestOffset, TempBase.first, 0, Len);
    return;
  }

  Type *ByteTy = IntegerType::get(NewFunc->getContext(), 8);

  Mem.setUnknown(DestBase, DestOffset, Len);

  auto &DestRegion = Mem.getRegion(DestBase);
  auto &SrcRegion = Mem.getRegion(SrcBase);
  uint64_t SrcEnd = SrcOffset + Len;
  BitVector Written(Len);
  for (auto &Store : make_range(SrcRegion.Ops.rbegin(), SrcRegion.Ops.rend())) {
    if (!Store.overlapsRange(SrcOffset, SrcEnd, DL)) {
      continue;
    }

    // Compute the affected portion, relative to the copied region.
    uint64_t StoreEnd = Store.getEndOffset(DL);
    uint64_t AffectedOffset = std::max(Store.Offset, SrcOffset);
    uint64_t AffectedEnd = std::min(StoreEnd, SrcEnd);
    if (AffectedOffset >= AffectedEnd) {
      continue;
    }

    uint64_t RelOffset = AffectedOffset - SrcOffset;
    uint64_t RelEnd = AffectedEnd - SrcOffset;

    if (Store.Kind == OpZero || Store.Kind == OpUnknown) {
      // Apply the same operation to every unwritten portion of `RelOffset ..
      // RelEnd`.
      int StartBit = Written.find_first_unset_in(RelOffset, RelEnd);
      while (StartBit != -1) {
        int EndBit = Written.find_first_in(StartBit, RelEnd);
        if (EndBit == -1) {
          EndBit = RelEnd;
        }
        MemStore NewStore = Store;
        NewStore.Offset = DestOffset + StartBit;
        NewStore.Len = EndBit - StartBit;
        DestRegion.pushOp(std::move(NewStore), DL);
        Written.set(StartBit, EndBit);

        StartBit = Written.find_first_unset_in(EndBit, RelEnd);
      }
    } else {
      assert(Store.Kind == OpStore);
      if (Store.containedByRange(SrcOffset, SrcEnd, DL) &&
          Written.find_first_in(RelOffset, RelEnd) == -1) {
        // The entire `Store` is in-range and not yet written.  We can copy it
        // directly into `DestRegion`.
        MemStore NewStore = Store;
        NewStore.Offset = DestOffset + RelOffset;
        DestRegion.pushOp(std::move(NewStore), DL);
        Written.set(RelOffset, RelEnd);
      } else {
        // Chop the stored value into bytes, and copy them one at a time.
        for (uint64_t I = RelOffset; I < RelEnd; ++I) {
          if (Written[I]) {
            continue;
          }

          uint64_t ByteOffset = I + SrcOffset - Store.Offset;
          Value *Byte = convertLoadResult(Store.Val, ByteOffset, ByteTy);
          SmallVector<Label, 8> Labels;
          if (Store.Labels.size() > I) {
            Labels.push_back(Store.Labels[I]);
          }
          MemStore NewStore =
              MemStore::CreateStore(DestOffset + I, Byte, std::move(Labels));
          DestRegion.pushOp(std::move(NewStore), DL);
          Written.set(I);
        }
      }
    }
  }
}

Value *State::foldInst(Instruction *Inst) {
  Constant *C = nullptr;
  Value *Simplified = SimplifyInstruction(Inst, SQ);
  if (Simplified == nullptr) {
    // `Inst` can't be simplified any further.  Try folding it to a constant
    // instead.
    C = constantFoldInstructionExtra(Inst);
  } else if (isa<Instruction>(Simplified)) {
    // Simplify never creates an instruction; it only ever returns existing
    // ones.  The existing instruction was already processed by constant
    // folding, so we know it can't be folded further.
    return Simplified;
  } else if (auto *SimpleConst = dyn_cast<Constant>(Simplified)) {
    // Constants are handled below, for both simplify and constant folding.
    C = SimpleConst;
  } else {
    errs() << "bad value kind after simplify: " << *Simplified << "\n";
    assert(0 && "bad value kind after simplify");
  }

  if (C != nullptr) {
    C = constantFoldExtra(C);
    return C;
  }

  return Inst;
}

SmallVector<Label, 8> State::labelInst(StackFrame *SF, Instruction *Inst) {
  // Handle each folded instruction.
  for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
    auto *Operand = Inst->getOperand(I);
    auto const &OperandL = SF->getLabels(Operand);
    for (unsigned J = 0; J < OperandL.size(); ++J) {
      if (OperandL[J] != BOTTOM) {
        return MaybeTaintedLabels;
      }
    }
  }

  return EmptyLabels;
}

void State::checkBottom(SmallVector<Label, 8> L) {
  for (unsigned I = 0; I < L.size(); ++I) {
    assert(L[I] == BOTTOM && "Invalid label. Must be BOTTOM.");
  }
}

Value *State::foldAndEmitInst(Instruction *Inst) {
  Value *V = foldInst(Inst);
  if (V == Inst) {
    NewBB->getInstList().push_back(Inst);
  } else {
    Inst->deleteValue();
  }
  return V;
}

Constant *State::constantFoldConstant(Constant *C) {
  Constant *Folded =
      ConstantFoldConstant(C, NewFunc->getParent()->getDataLayout(), TLI);
  if (Folded != nullptr) {
    return Folded;
  } else {
    return C;
  }
}

Constant *State::constantFoldExtra(Constant *C) {
  C = constantFoldAlignmentCheckAnd(C);
  C = constantFoldAlignmentCheckURem(C);
  C = constantFoldConstant(C);
  return C;
}

/// Fold alignment checks, like `(uintptr_t)ptr & 7`.  These appear in
/// functions like `memcpy` and `strcmp`.  We handle these by increasing the
/// alignment of the declaration of `ptr` so the result has a known value.
Constant *State::constantFoldAlignmentCheckAnd(Constant *C) {
  auto *And = dyn_cast<ConstantExpr>(C);
  if (And == nullptr || And->getOpcode() != Instruction::And) {
    return C;
  }

  Constant *Val = nullptr;
  ConstantInt *Mask = nullptr;
  if ((Mask = dyn_cast<ConstantInt>(And->getOperand(0)))) {
    Val = cast<Constant>(And->getOperand(1));
  } else if ((Mask = dyn_cast<ConstantInt>(And->getOperand(1)))) {
    Val = cast<Constant>(And->getOperand(0));
  } else {
    return C;
  }

  if (Mask->uge(4096)) {
    return C;
  }
  uint64_t MaskInt = Mask->getZExtValue();
  // Get the next power of two such that `Align > MaskInt`.
  uint64_t Align = 1 << (64 - __builtin_clz(MaskInt));

  // strcmp tries to be clever, and does two alignment checks at once via
  // `((ptr1 | ptr2) & 7) == 0`.  We handle this by reassociating the
  // expression as `(ptr1 & 7) | (ptr2 & 7)`, then fold it recursively.
  auto *ValOr = dyn_cast<ConstantExpr>(Val);
  if (ValOr != nullptr && ValOr->getOpcode() == Instruction::Or) {
    Constant *C0 =
        constantFoldExtra(ConstantExpr::getAnd(ValOr->getOperand(0), Mask));
    Constant *C1 =
        constantFoldExtra(ConstantExpr::getAnd(ValOr->getOperand(1), Mask));
    return constantFoldExtra(ConstantExpr::getOr(C0, C1));
  }

  // Evaluate `Val` as a pointer expression.
  LinearPtr *Ptr = evalPtr(Val);
  if (Ptr == nullptr) {
    return C;
  }
  return constantFoldAlignmentCheckPtr(C, Ptr, Align, MaskInt);
}

Constant *State::constantFoldAlignmentCheckURem(Constant *C) {
  auto *URem = dyn_cast<ConstantExpr>(C);
  if (URem == nullptr || URem->getOpcode() != Instruction::URem) {
    return C;
  }

  Constant *Val = URem->getOperand(0);
  ;
  ConstantInt *AlignConst = dyn_cast<ConstantInt>(URem->getOperand(1));
  if (AlignConst == nullptr) {
    return C;
  }

  if (AlignConst->uge(4096 + 1)) {
    return C;
  }
  uint64_t Align = AlignConst->getZExtValue();
  // Check if Align is a power of two.
  if ((Align & (Align - 1)) != 0) {
    return C;
  }

  // Evaluate `Val` as a pointer expression.
  LinearPtr *Ptr = evalPtr(Val);
  if (Ptr == nullptr) {
    return C;
  }
  return constantFoldAlignmentCheckPtr(C, Ptr, Align, Align - 1);
}

/// Handle an alignment check of the pointer `LP`.  The allocation is adjusted
/// to a multiple of `Align`, and the result is `Addr & Mask`.
Constant *State::constantFoldAlignmentCheckPtr(Constant *C, LinearPtr *LP,
                                               uint64_t Align, uint64_t Mask) {
  assert(Align > Mask);
  assert((Align & (Align - 1)) == 0 && "Align must be a power of two");

  for (auto &Term : LP->Terms) {
    // If the base is a global variable or function, adjust its alignment to at
    // least `Align`.
    auto *Global = dyn_cast<GlobalObject>(Term.Ptr);
    if (Global == nullptr) {
      return C;
    }

    unsigned OldAlign = Global->getAlignment();
    if (OldAlign < Align) {
      Global->setAlignment(MaybeAlign(Align));
    }
  }

  // Each term's base pointer is now equal to zero mod `Align`, so masking with
  // anything less than `Align` also produces zero.  The overall result is now
  // equal to `LP->Offset & Mask`.
  return ConstantInt::get(C->getType(), LP->Offset & Mask);
}

Constant *State::constantFoldInstructionExtra(Instruction *Inst) {
  Constant *C =
      ConstantFoldInstruction(Inst, NewFunc->getParent()->getDataLayout(), TLI);
  if (C != nullptr) {
    return C;
  }

  C = constantFoldNullCheckInst(Inst);
  if (C != nullptr) {
    return C;
  }

  C = constantFoldPointerCompare(Inst);
  if (C != nullptr) {
    return C;
  }

  return nullptr;
}

/// Null checks.  We handle `ptr == NULL` by evaluating a `ptr` to a
/// `LinearPtr` and checking if it's null.
Constant *State::constantFoldNullCheckInst(Instruction *Inst) {
  auto *ICmp = dyn_cast<ICmpInst>(Inst);
  if (ICmp == nullptr || !ICmp->isEquality()) {
    return nullptr;
  }

  Value *PtrVal = nullptr;
  Constant *NullConst;
  if ((NullConst = dyn_cast<Constant>(ICmp->getOperand(0))) &&
      NullConst->isNullValue()) {
    PtrVal = ICmp->getOperand(1);
  } else if ((NullConst = dyn_cast<Constant>(ICmp->getOperand(1))) &&
             NullConst->isNullValue()) {
    PtrVal = ICmp->getOperand(0);
  } else {
    return nullptr;
  }

  LinearPtr *Ptr = evalPtr(PtrVal);
  if (Ptr == nullptr) {
    return nullptr;
  }
  // Convert to base and offset form.  For now, we don't try to handle cases
  // where two pointers have been added together.
  Optional<Value *> OptBase = Ptr->getBase();
  if (!OptBase.hasValue()) {
    return nullptr;
  }
  Value *Base = OptBase.getValue();

  bool PtrIsNull = Base == nullptr && Ptr->Offset == 0;
  bool Result;
  if (ICmp->getPredicate() == CmpInst::ICMP_EQ) {
    Result = PtrIsNull;
  } else {
    assert(ICmp->getPredicate() == CmpInst::ICMP_NE);
    Result = !PtrIsNull;
  }

  if (Result) {
    return ConstantInt::getTrue(Inst->getContext());
  } else {
    return ConstantInt::getFalse(Inst->getContext());
  }
}

/// Equality checks between two pointers.  We handle `ptr1 == ptr2` by
/// evaluating both pointers to `LinearPtr`s and checking if their bases and
/// offsets are the same.
Constant *State::constantFoldPointerCompare(Instruction *Inst) {
  auto *ICmp = dyn_cast<ICmpInst>(Inst);
  if (ICmp == nullptr || ICmp->getPredicate() != CmpInst::ICMP_EQ) {
    return nullptr;
  }

  auto Ptr1 = evalBaseOffset(ICmp->getOperand(0));
  if (!Ptr1) {
    return nullptr;
  }
  auto Ptr2 = evalBaseOffset(ICmp->getOperand(1));
  if (!Ptr2) {
    return nullptr;
  }

  // Two pointers are equal if they have the same base and the same offset.
  bool Result = Ptr1->first == Ptr2->first && Ptr1->second == Ptr2->second;
  if (Result) {
    return ConstantInt::getTrue(Inst->getContext());
  } else {
    return ConstantInt::getFalse(Inst->getContext());
  }
}

Optional<std::pair<Value *, uint64_t>> State::evalBaseOffset(Value *V) {
  LinearPtr *LP = evalPtr(V);
  if (LP == nullptr) {
    return None;
  }
  Optional<Value *> Base = LP->getBase();
  if (!Base.hasValue()) {
    return None;
  }
  return std::make_pair(Base.getValue(), LP->Offset);
}

LinearPtr *State::evalPtr(Value *V) {
  auto It = EvalCache.find(V);
  if (It == EvalCache.end()) {
    Optional<LinearPtr> Ptr = evalPtrImpl(V);
    It = EvalCache.try_emplace(V, std::move(Ptr)).first;
  }
  if (It->second.hasValue()) {
    return &It->second.getValue();
  } else {
    return nullptr;
  }
}

Optional<LinearPtr> State::evalPtrImpl(Value *V) {
  if (auto *C = dyn_cast<Constant>(V)) {
    return evalPtrConstant(C);
  } else if (auto *Inst = dyn_cast<Instruction>(V)) {
    return evalPtrInstruction(Inst);
  } else if (isa<ConstantPointerNull>(V)) {
    return LinearPtr((uint64_t)0);
  } else {
    return None;
  }
}

Optional<LinearPtr> State::evalPtrConstant(Constant *C) {
  if (auto *Global = dyn_cast<GlobalObject>(C)) {
    return LinearPtr(C);
  } else if (auto *Alias = dyn_cast<GlobalAlias>(C)) {
    return evalPtrConstant(Alias->getAliasee());
  } else if (auto *Expr = dyn_cast<ConstantExpr>(C)) {
    return evalPtrOpcode(Expr->getOpcode(), Expr);
  } else if (auto *Int = dyn_cast<ConstantInt>(C)) {
    if (Int->getBitWidth() > 64) {
      // Don't convert if it will cause us to lose data.
      return None;
    }
    return LinearPtr(Int->getZExtValue());
  } else {
    return None;
  }
}

Optional<LinearPtr> State::evalPtrInstruction(Instruction *Inst) {
  return evalPtrOpcode(Inst->getOpcode(), Inst);
}

Optional<LinearPtr> State::evalPtrOpcode(unsigned Opcode, User *U) {
  LinearPtr *A = nullptr;
  LinearPtr *B = nullptr;
  switch (Opcode) {
  case Instruction::Add:
    A = evalPtr(U->getOperand(0));
    B = evalPtr(U->getOperand(1));
    if (A == nullptr || B == nullptr) {
      return None;
    }
    return A->add(*B);

  case Instruction::Sub:
    A = evalPtr(U->getOperand(0));
    B = evalPtr(U->getOperand(1));
    if (A == nullptr || B == nullptr) {
      return None;
    }
    return A->sub(*B);

  case Instruction::Mul:
    A = evalPtr(U->getOperand(0));
    B = evalPtr(U->getOperand(1));
    if (A == nullptr || B == nullptr) {
      return None;
    }
    return A->mul(*B);

  case Instruction::IntToPtr:
  case Instruction::PtrToInt:
  case Instruction::BitCast:
    A = evalPtr(U->getOperand(0));
    if (A == nullptr) {
      return None;
    }
    return *A;

  case Instruction::GetElementPtr:
    return std::move(evalPtrGEP(U));

  default:
    return None;
  }
}

Optional<LinearPtr> State::evalPtrGEP(User *U) {
  // Get the pointee type for the base of the GEP.  Note the cast can fail,
  // since GEP works on vectors of pointers as well as ordinary pointers.
  auto *BasePtrTy = dyn_cast<PointerType>(U->getOperand(0)->getType());
  if (BasePtrTy == nullptr) {
    return None;
  }
  Type *BaseTy = BasePtrTy->getPointerElementType();

  // Get the base pointer as a `LinearPtr`.
  LinearPtr *BaseLP = evalPtr(U->getOperand(0));
  if (BaseLP == nullptr) {
    return None;
  }
  LinearPtr Result = *BaseLP;

  DataLayout const &DL = NewFunc->getParent()->getDataLayout();

  // Apply the first offset, which does pointer arithmeon to the base pointer.
  auto *Idx0 = dyn_cast<ConstantInt>(U->getOperand(1));
  if (Idx0 == nullptr) {
    return None;
  }
  Result.Offset += DL.getTypeAllocSize(BaseTy) * Idx0->getSExtValue();

  Type *CurTy = BaseTy;
  for (unsigned I = 2; I < U->getNumOperands(); ++I) {
    auto *Idx = dyn_cast<ConstantInt>(U->getOperand(I));
    if (Idx == nullptr) {
      return None;
    }
    int64_t IdxVal = Idx->getSExtValue();

    if (auto *StructTy = dyn_cast<StructType>(CurTy)) {
      Result.Offset += DL.getStructLayout(StructTy)->getElementOffset(IdxVal);
      CurTy = StructTy->getElementType(IdxVal);
    } else if (auto *ArrayTy = dyn_cast<ArrayType>(CurTy)) {
      Result.Offset += DL.getTypeAllocSize(ArrayTy->getElementType()) * IdxVal;
      CurTy = ArrayTy->getElementType();
    } else {
      return None;
    }
  }

  return std::move(Result);
}

// Unwinding

std::string valueName(Value *V) {
  if (!V->getName().empty()) {
    return V->getName().str();
  }

  std::string S;
  raw_string_ostream Out(S);
  V->printAsOperand(Out, false);

  auto Parts = StringRef(Out.str()).rsplit('%');
  if (!Parts.second.empty()) {
    if (isa<BasicBlock>(V)) {
      return Twine("bb", Parts.second).str();
    } else if (isa<Instruction>(V)) {
      return Twine("v", Parts.second).str();
    } else if (isa<Argument>(V)) {
      return Twine("a", Parts.second).str();
    }
  }
  return Out.str();
}

void State::unwind() {
  assert(Stack.size() > 0);

  // Stack of calling contexts.  There's an `UnwindContext` for every frame
  // except the innermost one.
  std::vector<UnwindContext> UnwindStack;
  for (unsigned I = 0; I < Stack.size() - 1; ++I) {
    StackFrame &SF = Stack[I];
    UnwindContext UC;
    if (I > 0) {
      UC = UnwindStack.back();
    }

    errs() << "build unwind context for " << SF.Func.getName() << "\n";

    UC.ReturnDest =
        BasicBlock::Create(NewFunc->getContext(),
                           Twine(SF.Func.getName(), ".returndest"), NewFunc);
    Type *ReturnType = Stack[I + 1].Func.getReturnType();
    if (!ReturnType->isVoidTy()) {
      Value *PHI = PHINode::Create(ReturnType, 0, "returnval", UC.ReturnDest);
      if (SF.CastReturnType != nullptr) {
        PHI = foldAndEmitInst(CastInst::CreateBitOrPointerCast(
            PHI, SF.CastReturnType, "returncast"));
      }
      assert(SF.ReturnValue != nullptr);
      SF.Locals[SF.ReturnValue] = PHI;
    }

    if (SF.UnwindDest != nullptr) {
      UC.UnwindDest =
          BasicBlock::Create(NewFunc->getContext(),
                             Twine(SF.Func.getName(), ".unwinddest"), NewFunc);

      LandingPadInst *Pad = cast<LandingPadInst>(&SF.UnwindDest->front());
      UC.LandingPads.push_back(Pad);
      Value *PHI =
          PHINode::Create(Pad->getType(), 0, "unwindval", UC.UnwindDest);

      SF.Locals[Pad] = PHI;
    }

    UnwindStack.push_back(std::move(UC));
  }

  while (Stack.size() > 0) {
    unsigned I = Stack.size() - 1;

    UnwindContext *PrevUC = nullptr;
    if (I > 0 && I - 1 < UnwindStack.size()) {
      PrevUC = &UnwindStack[I - 1];
    }

    UnwindContext *UC = nullptr;
    if (I < UnwindStack.size()) {
      UC = &UnwindStack[I];
    }

    unwindFrame(Stack[I], PrevUC, UC);

    Stack.pop_back();
    if (UC != nullptr) {
      UnwindStack.pop_back();
    }
  }
}

struct UnwindFrameState {
  State &S;
  StackFrame &SF;
  UnwindContext *PrevUC;
  DominatorTree *DT;

  DenseMap<BasicBlock *, BasicBlock *> BlockMap;
  /// For each new block, a map from old values to new values.  This must be
  /// tracked per-block (instead of everything using `SF.Locals`) because value
  /// mappings can be overwritten when a partial block is in a loop.  For each
  /// block, the `BlockLocals` are initialized to the final `BlockLocals` of
  /// some predecessor (it doesn't matter which one, since only values common
  /// to all of them can actually be used), and then the map is updated
  /// incrementally as instructions are emitted into the block.
  ///
  /// Keying on the new block instead of the old one lets us track partial and
  /// full versions of the same old block separately.
  DenseMap<BasicBlock *, DenseMap<Value *, Value *>> BlockLocals;
  /// List of old blocks that need to be converted.  Every block in this list
  /// will also be present as a key in BlockMap.
  std::vector<BasicBlock *> Pending;
  /// List of all predecessors, as pairs of `OldPred` and `NewPred`.  For each
  /// predecessor, `handlePHINodes` will update the phi nodes of all its
  /// possible successors.
  std::vector<std::pair<BasicBlock *, BasicBlock *>> AllPreds;

  /// Map from old phi nodes to corresponding new ones.  We can't use
  /// `SF.Locals` for this because some old phi nodes are mapped to trampoline
  /// block phi nodes in that map.
  DenseMap<PHINode *, PHINode *> PHINodeMap;

  /// Trampolines used for exiting partial blocks.  Partial blocks need special
  /// handling because a separate full clone of the block might also exist if
  /// the original block is inside a loop.  We handle this by splitting the
  /// partial block just before the terminator and placing adding phi nodes for
  /// everything it defines into the trampoline block.
  ///
  /// The key in this map is the old block, and the value is the trampoline
  /// block.  The trampoline block consists of a sequence of PHINodes (one for
  /// each value defined in the old block, in order) followed by a clone of the
  /// old terminator.
  DenseMap<BasicBlock *, BasicBlock *> ExitTrampolines;

  UnwindFrameState(State &S, StackFrame &SF, UnwindContext *PrevUC)
      : S(S), SF(SF), PrevUC(PrevUC) {
    DT = &S.P.getAnalysis<DominatorTreeWrapperPass>(SF.Func).getDomTree();
  }

  void emitInst(Instruction *Inst, BasicBlock *Out);
  void emitFullBlock(BasicBlock *BB, BasicBlock *Out);
  void emitPartialBlock(BasicBlock *BB, BasicBlock::iterator Iter,
                        BasicBlock *Out);
  void emitAllBlocks();

  void handlePHINodes();

  /// Map an old value to the corresponding new one.  This returns constants
  /// unchanged, and otherwise maps values through `BlockLocals[NewBB]`.  It
  /// aborts if `OldVal` is not a constant and isn't present in the map either.
  Value *mapValue(Value *OldVal, BasicBlock *NewBB);
  /// Map an old basic block to the corresponding new one.  This creates the
  /// new block if needed.  When creating a new block, this initializes the new
  /// block's `BlockLocals` entry with a copy of `BlockLocals[NewPred]`.
  BasicBlock *mapBlock(BasicBlock *OldBB, BasicBlock *NewPred);
};

void State::unwindFrame(StackFrame &SF, UnwindContext *PrevUC,
                        UnwindContext *UC) {
  errs() << "unwinding out of " << SF.Func.getName() << "\n";
  UnwindFrameState UFS(*this, SF, PrevUC);

  if (UC == nullptr) {
    // This is the innermost frame.  Emit the remaining instructions of the
    // current block directly into `NewBB`.  Other blocks will be generated as
    // needed.
    UFS.BlockLocals[NewBB] = SF.Locals;
    UFS.emitPartialBlock(SF.CurBB, SF.Iter, NewBB);
  } else {
    UFS.BlockLocals[UC->ReturnDest] = SF.Locals;
    UFS.emitPartialBlock(SF.CurBB, SF.Iter, UC->ReturnDest);
  }

  if (SF.UnwindDest != nullptr) {
    BasicBlock::iterator Iter = std::next(SF.UnwindDest->begin(), 1);
    UFS.BlockLocals[UC->UnwindDest] = SF.Locals;
    UFS.emitPartialBlock(SF.UnwindDest, Iter, UC->UnwindDest);
  }

  UFS.emitAllBlocks();

  UFS.handlePHINodes();
}

void UnwindFrameState::emitInst(Instruction *Inst, BasicBlock *Out) {
  if (auto *PHI = dyn_cast<PHINode>(Inst)) {
    // Add a new empty PHI node of the same type.  Incoming values will be
    // populated in a postprocessing pass, based on the edges actually
    // traversed.  (E.g. if we unwind in the middle of one side of a
    // conditional, the new merge block may have only one predecessor instead
    // of two.)
    PHINode *NewPHI = PHINode::Create(
        PHI->getType(), PHI->getNumIncomingValues(), PHI->getName(), Out);
    BlockLocals[Out][PHI] = NewPHI;
    PHINodeMap[PHI] = NewPHI;
    return;
  }

  if (auto *Return = dyn_cast<ReturnInst>(Inst)) {
    if (PrevUC != nullptr) {
      BranchInst::Create(PrevUC->ReturnDest, Out);
      Value *OldVal = Return->getReturnValue();
      if (OldVal != nullptr && !OldVal->getType()->isVoidTy()) {
        PHINode *PHI = cast<PHINode>(&*PrevUC->ReturnDest->begin());
        Value *NewVal = mapValue(Return->getReturnValue(), Out);
        PHI->addIncoming(NewVal, Out);
      }
      return;
    }
  }

  if (auto *Resume = dyn_cast<ResumeInst>(Inst)) {
    if (PrevUC != nullptr && PrevUC->UnwindDest != nullptr) {
      BranchInst::Create(PrevUC->UnwindDest, Out);
      PHINode *PHI = cast<PHINode>(&*PrevUC->UnwindDest->begin());
      Value *NewVal = mapValue(Resume->getValue(), Out);
      PHI->addIncoming(NewVal, Out);
      return;
    }
  }

  Instruction *NewInst = Inst->clone();
  if (!Inst->getType()->isVoidTy()) {
    NewInst->setName(Twine(Inst->getFunction()->getName()) + "." +
                     valueName(Inst));
  }
  Out->getInstList().push_back(NewInst);

  for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
    Value *OldVal = Inst->getOperand(I);
    if (!isa<BasicBlock>(OldVal)) {
      Value *NewVal = mapValue(OldVal, Out);
      NewInst->setOperand(I, NewVal);
    }
    // Basic blocks are handled below, after setting `BlockLocals[Out][Inst]`.
  }

  if (auto *LandingPad = dyn_cast<LandingPadInst>(Inst)) {
    if (PrevUC != nullptr) {
      // Accumulate clauses from enclosing landing pads.  According to the LLVM
      // exception handling docs, this is roughly what LLVM does when inlining.
      unsigned TotalNewClauses = 0;
      for (auto *EnclosingPad : PrevUC->LandingPads) {
        TotalNewClauses += EnclosingPad->getNumClauses();
      }
      LandingPad->reserveClauses(TotalNewClauses);
      for (auto *EnclosingPad : PrevUC->LandingPads) {
        for (unsigned I = 0; I < EnclosingPad->getNumClauses(); ++I) {
          LandingPad->addClause(EnclosingPad->getClause(I));
        }
        if (EnclosingPad->isCleanup()) {
          LandingPad->setCleanup(true);
        }
      }
    }
  }

  if (Inst->isEHPad() || Inst->isExceptionalTerminator()) {
    // Exception-related instructions can only appear inside a function with an
    // EH personality.
    Function *OldFunc = Inst->getFunction();
    assert(OldFunc->hasPersonalityFn());
    Constant *Personality = OldFunc->getPersonalityFn();
    if (!S.NewFunc->hasPersonalityFn()) {
      S.NewFunc->setPersonalityFn(Personality);
    } else {
      assert(S.NewFunc->getPersonalityFn() == Personality &&
             "mismatch between different personality functions");
    }
  }

  BlockLocals[Out][Inst] = NewInst;

  // We call `mapBlock` only after setting `BlockLocals[Out][Inst]` so that the
  // successor block will inherit the mapping for `Inst`.
  for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
    Value *OldVal = Inst->getOperand(I);
    if (auto *OldBB = dyn_cast<BasicBlock>(OldVal)) {
      BasicBlock *NewBB = mapBlock(OldBB, Out);
      NewInst->setOperand(I, NewBB);
    }
  }
}

void UnwindFrameState::emitFullBlock(BasicBlock *BB, BasicBlock *Out) {
  auto TrampIter = ExitTrampolines.find(BB);
  BasicBlock *TrampBB = nullptr;
  if (TrampIter != ExitTrampolines.end()) {
    TrampBB = TrampIter->second;
  }

  if (TrampBB == nullptr) {
    AllPreds.emplace_back(BB, Out);
  }
  // If TrampBB is non-null, the code that created the trampoline already added
  // it to AllPreds.

  for (Instruction &Inst : *BB) {
    if (TrampBB != nullptr && Inst.isTerminator()) {
      auto Iter = TrampBB->begin();
      DomTreeNode *Node = DT->getNode(BB);
      while (Node != nullptr) {
        for (Instruction &Inst : *Node->getBlock()) {
          if (Inst.isTerminator() || Inst.getType()->isVoidTy()) {
            continue;
          }
          PHINode *PHI = cast<PHINode>(&*Iter);
          ++Iter;
          PHI->addIncoming(mapValue(&Inst, Out), Out);
        }
        Node = Node->getIDom();
      }

      BranchInst::Create(TrampBB, Out);
      return;
    }
    emitInst(&Inst, Out);
  }
}

void UnwindFrameState::emitPartialBlock(BasicBlock *BB,
                                        BasicBlock::iterator Iter,
                                        BasicBlock *Out) {
  for (Instruction &Inst :
       iterator_range<BasicBlock::iterator>(Iter, BB->end())) {
    if (Inst.isTerminator()) {
      break;
    }
    emitInst(&Inst, Out);
  }

  // Build trampoline block.  This block contains a phi node for every value
  // visible downstream, in case it's possible to reenter BB as a full block.
  BasicBlock *TrampBB = BasicBlock::Create(
      S.NewFunc->getContext(),
      Twine(BB->getParent()->getName()) + "." + valueName(BB) + ".trampoline",
      S.NewFunc);
  AllPreds.emplace_back(BB, TrampBB);
  // FIXME: remove.  BlockLocals[TrampBB] should start empty, so only the phi
  // nodes created below are visible
  // Note `Map[X] = Map[Y]` can invalidate `Map[Y]` if `X` is not in the map.
  auto TempLocals = BlockLocals[Out];
  BlockLocals[TrampBB] = TempLocals;

  DomTreeNode *Node = DT->getNode(BB);
  while (Node != nullptr) {
    for (Instruction &Inst : *Node->getBlock()) {
      if (Inst.isTerminator() || Inst.getType()->isVoidTy()) {
        continue;
      }
      PHINode *PHI =
          PHINode::Create(Inst.getType(), 1, Inst.getName(), TrampBB);
      PHI->addIncoming(mapValue(&Inst, Out), Out);
      BlockLocals[TrampBB][&Inst] = PHI;
    }
    Node = Node->getIDom();
  }

  BranchInst::Create(TrampBB, Out);
  emitInst(BB->getTerminator(), TrampBB);
  ExitTrampolines.insert(std::make_pair(BB, TrampBB));
}

void UnwindFrameState::emitAllBlocks() {
  while (Pending.size() > 0) {
    BasicBlock *BB = Pending.back();
    Pending.pop_back();

    // Everything we find in `Pending` should already have a new block created,
    // so passing `nullptr` for the predecessor is okay here.
    BasicBlock *Out = mapBlock(BB, nullptr);
    emitFullBlock(BB, Out);
  }
}

void UnwindFrameState::handlePHINodes() {
  for (auto &Entry : AllPreds) {
    BasicBlock *OldPred = Entry.first;
    BasicBlock *NewPred = Entry.second;

    Instruction *Term = OldPred->getTerminator();
    for (unsigned I = 0; I < Term->getNumSuccessors(); ++I) {
      BasicBlock *OldSucc = Term->getSuccessor(I);
      for (PHINode &OldPHI : OldSucc->phis()) {
        PHINode *NewPHI = PHINodeMap[&OldPHI];
        assert(NewPHI != nullptr && "missing entry for phi node in PHINodeMap");
        Value *OldVal = OldPHI.getIncomingValueForBlock(OldPred);
        Value *NewVal = mapValue(OldVal, NewPred);
        NewPHI->addIncoming(NewVal, NewPred);
      }
    }
  }
}

Value *UnwindFrameState::mapValue(Value *OldVal, BasicBlock *NewBB) {
  if (isa<Constant>(OldVal)) {
    return OldVal;
  }

  auto &Locals = BlockLocals[NewBB];
  auto It = Locals.find(OldVal);
  if (It == Locals.end()) {
    errs() << "error: no local mapping for value " << *OldVal
           << " in new block ";
    NewBB->printAsOperand(errs());
    errs() << "\n";
    assert(0 && "no local mapping for value");
  }
  return It->second;
}

BasicBlock *UnwindFrameState::mapBlock(BasicBlock *OldBB, BasicBlock *NewPred) {
  auto It = BlockMap.find(OldBB);
  if (It != BlockMap.end()) {
    return It->second;
  }

  BasicBlock *NewBB = BasicBlock::Create(
      S.NewFunc->getContext(),
      Twine(OldBB->getParent()->getName()) + "." + valueName(OldBB), S.NewFunc);
  BlockMap.insert(std::make_pair(OldBB, NewBB));
  assert(NewPred != nullptr);
  // Note `Map[X] = Map[Y]` can invalidate `Map[Y]` if `X` is not in the map.
  auto TempLocals = BlockLocals[NewPred];
  BlockLocals[NewBB] = TempLocals;
  Pending.push_back(OldBB);
  return NewBB;
}

// Memory update

void State::updateMemory() {
  unsigned Count = 0;
  unsigned CountUnmodified = 0;

  // Temporary storage.  We `memCopy` each region into this buffer, relying on
  // the fact that `memCopy` produces a region with strictly non-overlapping
  // memory ops.
  auto Temp =
      allocateGlobal(IntegerType::get(NewFunc->getContext(), 8), 0, 1, false);
  GlobalVariable *TempBase = Temp.first;
  // Add `TempBase` to `Mem.Regions` preemptively, so we can access it inside
  // the loop without invalidating the iterator.
  Mem.getRegion(TempBase);

  Type *ByteTy = IntegerType::get(NewFunc->getContext(), 8);
  Constant *ZeroByte = ConstantInt::get(ByteTy, 0);
  Constant *UndefByte = UndefValue::get(ByteTy);

  std::vector<Constant *> Fields;
  std::vector<std::pair<uint64_t, Label>> Labels;
  std::vector<std::pair<GlobalVariable *, Constant *>> Replacements;
  for (auto &Entry : Mem.Regions) {
    if (Entry.first == TempBase) {
      continue;
    }
    Value *Base = Entry.first;
    MemRegion &Region = Entry.second;

    ++Count;
    if (!Region.Written) {
      ++CountUnmodified;
      continue;
    }

    auto *GV = dyn_cast<GlobalVariable>(Base);
    if (GV == nullptr) {
      errs() << "alloc base is not a global variable: " << *Base << "\n";
      assert(0 && "alloc base is not a global variable");
    }

    uint64_t Size = DL.getTypeAllocSize(GV->getValueType());
    memCopy(TempBase, 0, GV, 0, Size);
    MemRegion &TempRegion = Mem.getRegion(TempBase);
    std::sort(TempRegion.Ops.begin(), TempRegion.Ops.end(),
              [](MemStore const &A, MemStore const &B) {
                return A.Offset < B.Offset;
              });

    // For each region, we build a `ConstantStruct` exactly matching the layout
    // and value found in the `MemRegion`.
    uint64_t Pos = 0;
    for (auto &Op : TempRegion.Ops) {
      if (Op.Offset >= Size) {
        errs() << "warning: region ";
        GV->printAsOperand(errs());
        errs() << " contains out-of-bounds write at " << Op.Offset
               << " (size = " << Size << ")\n";
        continue;
      }

      if (Pos > Op.Offset) {
        errs() << "region ";
        GV->printAsOperand(errs());
        errs() << " has overlapping writes: found offset = " << Op.Offset
               << " when pos = " << Pos << "\n";
        assert(0 && "region has overlapping writes");
      }

      while (Pos < Op.Offset) {
        Fields.push_back(UndefByte);
        Pos += 1;
      }

      switch (Op.Kind) {
      case OpStore: {
        auto *C = dyn_cast<Constant>(Op.Val);
        if (C == nullptr) {
          errs() << "region ";
          GV->printAsOperand(errs());
          errs() << " contains non-constant value " << *Op.Val
                 << " (size = " << Size << ")\n";
          assert(0 && "region contains non-constant value");
        }
        Fields.push_back(C);
        Pos += DL.getTypeAllocSize(C->getType());
      } break;
      case OpZero: {
        for (unsigned I = 0; I < Op.Len; ++I) {
          Fields.push_back(ZeroByte);
          Pos += 1;
        }
      } break;
      case OpUnknown: {
        for (unsigned I = 0; I < Op.Len; ++I) {
          Fields.push_back(UndefByte);
          Pos += 1;
        }
      } break;
      }

      for (unsigned I = 0; I < Op.Labels.size(); ++I) {
        if (Op.Labels[I] != BOTTOM) {
          Labels.push_back(std::make_pair(Op.Offset + I, Op.Labels[I]));
        }
      }
    }
    while (Pos < Size) {
      Fields.push_back(UndefByte);
      Pos += 1;
    }
    assert(Pos == Size);

    Constant *Struct = ConstantStruct::getAnon(NewFunc->getContext(), Fields);
    Fields.clear();
    TempRegion.Ops.clear();
    TempRegion.MinOffset = 0;
    TempRegion.MaxOffset = 0;

    // Create a new `GlobalVariable` with the new type and initializer, and
    // replace all uses of `GV` with it.
    GlobalVariable *NewGV = new GlobalVariable(
        *NewFunc->getParent(), Struct->getType(), GV->isConstant(),
        GV->getLinkage(), Struct, GV->getName(), GV, GV->getThreadLocalMode(),
        GV->getAddressSpace(), GV->isExternallyInitialized());
    NewGV->copyAttributesFrom(GV);
    Constant *BitCast = ConstantExpr::getBitCast(NewGV, GV->getType());
    Replacements.push_back(std::make_pair(GV, BitCast));

    // Emit `noniSetLabelU8` calls to initialize taint labels in `NewGV`.
    if (Labels.size() > 0) {
      Type *IntTy = IntegerType::get(NewFunc->getContext(), 32);
      FunctionCallee SetLabelFunc = NewFunc->getParent()->getOrInsertFunction(
          "noniSetLabelU8", Type::getVoidTy(NewFunc->getContext()),
          ByteTy->getPointerTo(), IntTy);
      Instruction *FirstInst = NewBB->getFirstNonPHI();

      Constant *Ptr = ConstantExpr::getBitCast(NewGV, ByteTy->getPointerTo());
      for (auto &Entry : Labels) {
        Value *Args[2] = {
            ConstantExpr::getGetElementPtr(
                ByteTy, Ptr, ConstantInt::get(IntTy, Entry.first), true),
            ConstantInt::get(IntTy, Entry.second),
        };
        CallInst::Create(SetLabelFunc, Args, "", FirstInst);
      }
      Labels.clear();
    }
  }

  for (auto &Repl : Replacements) {
    Repl.first->replaceAllUsesWith(Repl.second);
    Repl.first->eraseFromParent();
  }

  TempBase->eraseFromParent();

  errs() << "processed " << Count << " memory regions (" << CountUnmodified
         << " unmodified)\n";
}

// LLVM pass

struct FlattenInit : public ModulePass {
  static char ID;
  FlattenInit() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    Function *MainFunc = M.getFunction("main");
    if (MainFunc == nullptr || MainFunc->isDeclaration()) {
      return false;
    }

    TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>(*MainFunc).getTLI(*MainFunc);

    MainFunc->setName("__cc_old_main");
    State S(*this, *MainFunc, "main", TLI);
    S.run();

    for (auto &BB : *S.NewFunc) {
      for (auto &Inst : BB) {
        for (Value *V : Inst.operand_values()) {
          if (auto *OpBB = dyn_cast<BasicBlock>(V)) {
            if (OpBB->getParent() != S.NewFunc) {
              errs() << "INVALID: inst " << Inst << " references basic block "
                     << OpBB << " " << OpBB->getName() << " of function "
                     << OpBB->getParent()->getName() << "\n";
            }
          } else if (auto *OpInst = dyn_cast<Instruction>(V)) {
            if (OpInst->getFunction() != S.NewFunc) {
              errs() << "INVALID: inst " << Inst << " references instruction "
                     << *OpInst << " of function "
                     << OpInst->getFunction()->getName() << "\n";
            }
          }
        }
      }
    }

    MainFunc->eraseFromParent();

    return true;
  }

private:
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
  }
}; // end of struct FlattenInit
} // end of anonymous namespace

char FlattenInit::ID = 0;
static RegisterPass<FlattenInit>
    X("flatten-init", "Inline and unroll constant-valued initialization code",
      false /* Only looks at CFG */, false /* Analysis Pass */);
