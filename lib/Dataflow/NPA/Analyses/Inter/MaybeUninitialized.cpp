/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"

#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace npa {

namespace {

class UninitializedInfo {
public:
  explicit UninitializedInfo(llvm::Module &M) { buildUniverse(M); }

  unsigned getBitWidth() const { return nextBit == 0 ? 1 : nextBit; }

  unsigned getValueBit(const llvm::Value *V) const {
    auto It = valueBits.find(V);
    if (It == valueBits.end())
      return invalidBit();
    return It->second;
  }

  unsigned getMemoryBit(const llvm::Value *V) const {
    const llvm::Value *Mem = normalizeMemory(V);
    auto It = memoryBits.find(Mem);
    if (It == memoryBits.end())
      return invalidBit();
    return It->second;
  }

  static unsigned invalidBit() { return static_cast<unsigned>(-1); }

private:
  unsigned nextBit = 0;
  std::unordered_map<const llvm::Value *, unsigned> valueBits;
  std::unordered_map<const llvm::Value *, unsigned> memoryBits;

  static const llvm::Value *normalizeMemory(const llvm::Value *V) {
    if (!V || !V->getType()->isPointerTy())
      return nullptr;
    return llvm::getUnderlyingObject(V->stripPointerCasts());
  }

  void addValueBit(const llvm::Value *V) {
    if (!V || valueBits.count(V))
      return;
    valueBits.emplace(V, nextBit++);
  }

  void addMemoryBit(const llvm::Value *V) {
    const llvm::Value *Mem = normalizeMemory(V);
    if (!Mem || memoryBits.count(Mem))
      return;
    memoryBits.emplace(Mem, nextBit++);
  }

  void buildUniverse(llvm::Module &M) {
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      for (auto &Arg : F.args()) {
        addValueBit(&Arg);
        addMemoryBit(&Arg);
      }
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.getType()->isVoidTy())
            addValueBit(&I);
          if (llvm::isa<llvm::AllocaInst>(&I))
            addMemoryBit(&I);
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            addMemoryBit(Load->getPointerOperand());
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
            addMemoryBit(Store->getPointerOperand());
          if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            for (llvm::Use &Arg : Call->args())
              addMemoryBit(Arg.get());
          }
        }
      }
    }
  }
};

class MaybeUninitializedAnalysis {
public:
  using FactType = llvm::APInt;
  using D = TaintTransformer;
  using Exp = Exp0<D>;
  using E = E0<D>;

  explicit MaybeUninitializedAnalysis(llvm::Module &M)
      : info(M), bitWidth(info.getBitWidth()), widthScope(bitWidth) {}

  FactType getEntryValue() const { return llvm::APInt(bitWidth, 0); }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;

    D::value_type transfer = D::one();
    bool updated = false;

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
      unsigned valueBit = info.getValueBit(Alloca);
      if (valueBit != UninitializedInfo::invalidBit()) {
        assignZero(transfer, valueBit);
        updated = true;
      }
      unsigned memBit = info.getMemoryBit(Alloca);
      if (memBit != UninitializedInfo::invalidBit()) {
        assignGen(transfer, memBit);
        updated = true;
      }
    } else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      updated |= assignFromValue(transfer,
                                 info.getMemoryBit(Store->getPointerOperand()),
                                 Store->getValueOperand());
    } else if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      unsigned srcBit = info.getMemoryBit(Load->getPointerOperand());
      unsigned dstBit = info.getValueBit(Load);
      updated |= assignFromSources(transfer, dstBit, {srcBit}, false);
    } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      updated |= assignFromOperands(transfer, info.getValueBit(Cast),
                                    {Cast->getOperand(0)});
    } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
      updated |= assignFromOperands(transfer, info.getValueBit(GEP),
                                    {GEP->getPointerOperand()});
    } else if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      updated |=
          assignFromOperands(transfer, info.getValueBit(Select),
                             {Select->getCondition(), Select->getTrueValue(),
                              Select->getFalseValue()});
    } else if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      std::vector<const llvm::Value *> incoming;
      incoming.reserve(Phi->getNumIncomingValues());
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
        incoming.push_back(Phi->getIncomingValue(i));
      updated |= assignFromOperands(transfer, info.getValueBit(Phi), incoming);
    } else if (auto *Cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      updated |= assignFromOperands(transfer, info.getValueBit(Cmp),
                                    {Cmp->getOperand(0), Cmp->getOperand(1)});
    } else if (auto *BinOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      updated |=
          assignFromOperands(transfer, info.getValueBit(BinOp),
                             {BinOp->getOperand(0), BinOp->getOperand(1)});
    } else if (!I.getType()->isVoidTy()) {
      unsigned dstBit = info.getValueBit(&I);
      if (dstBit != UninitializedInfo::invalidBit()) {
        assignZero(transfer, dstBit);
        updated = true;
      }
    }

    if (!updated)
      return currentPath;
    return Exp::seq(transfer, currentPath);
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &Call,
                                     const llvm::Function &Callee) {
    D::value_type transfer = D::one();
    const size_t count = std::min<size_t>(
        Call.arg_size(), static_cast<size_t>(Callee.arg_size()));
    const auto *ParamIt = Callee.arg_begin();
    for (size_t i = 0; i < count; ++i, ++ParamIt) {
      assignFromValue(transfer, info.getValueBit(&*ParamIt),
                      Call.getArgOperand(i));
      assignPointerMemory(transfer, info.getMemoryBit(&*ParamIt),
                          Call.getArgOperand(i));
    }
    return transfer;
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &Call,
                                      const llvm::Function &Callee) {
    D::value_type transfer = D::one();

    if (!Call.getType()->isVoidTy()) {
      std::vector<unsigned> retBits;
      bool gen = false;
      for (const auto &BB : Callee) {
        if (const auto *Ret =
                llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
          const llvm::Value *RetVal = Ret->getReturnValue();
          if (llvm::isa<llvm::UndefValue>(RetVal))
            gen = true;
          unsigned bit = info.getValueBit(RetVal);
          if (bit != UninitializedInfo::invalidBit())
            retBits.push_back(bit);
        }
      }
      assignFromSources(transfer, info.getValueBit(&Call), retBits, gen);
    }

    const size_t count = std::min<size_t>(
        Call.arg_size(), static_cast<size_t>(Callee.arg_size()));
    const auto *ParamIt = Callee.arg_begin();
    for (size_t i = 0; i < count; ++i, ++ParamIt)
      assignPointerMemory(transfer, info.getMemoryBit(Call.getArgOperand(i)),
                          &*ParamIt);

    return transfer;
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) {
    D::value_type transfer = D::one();
    if (!Call.getType()->isVoidTy())
      assignZero(transfer, info.getValueBit(&Call));
    return transfer;
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact) {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) {
    return lhs | rhs;
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) {
    return lhs == rhs;
  }

private:
  UninitializedInfo info;
  unsigned bitWidth = 1;
  D::WidthScope widthScope;

  void clearDestination(D::value_type &transfer, unsigned destBit) const {
    if (destBit == UninitializedInfo::invalidBit())
      return;
    for (auto &row : transfer.rel)
      row.clearBit(destBit);
    transfer.gen.clearBit(destBit);
  }

  void assignZero(D::value_type &transfer, unsigned destBit) const {
    if (destBit == UninitializedInfo::invalidBit())
      return;
    clearDestination(transfer, destBit);
  }

  void assignGen(D::value_type &transfer, unsigned destBit) const {
    if (destBit == UninitializedInfo::invalidBit())
      return;
    clearDestination(transfer, destBit);
    D::addGen(transfer, destBit);
  }

  bool assignFromSources(D::value_type &transfer, unsigned destBit,
                         const std::vector<unsigned> &sourceBits,
                         bool gen) const {
    if (destBit == UninitializedInfo::invalidBit())
      return false;
    clearDestination(transfer, destBit);
    for (unsigned bit : sourceBits) {
      if (bit != UninitializedInfo::invalidBit())
        D::addEdge(transfer, bit, destBit);
    }
    if (gen)
      D::addGen(transfer, destBit);
    return true;
  }

  bool assignFromValue(D::value_type &transfer, unsigned destBit,
                       const llvm::Value *V) const {
    bool gen = llvm::isa<llvm::UndefValue>(V);
    unsigned bit = info.getValueBit(V);
    std::vector<unsigned> bits;
    if (bit != UninitializedInfo::invalidBit())
      bits.push_back(bit);
    return assignFromSources(transfer, destBit, bits, gen);
  }

  bool assignPointerMemory(D::value_type &transfer, unsigned destBit,
                           const llvm::Value *PtrLike) const {
    unsigned bit = info.getMemoryBit(PtrLike);
    std::vector<unsigned> bits;
    if (bit != UninitializedInfo::invalidBit())
      bits.push_back(bit);
    return assignFromSources(transfer, destBit, bits, false);
  }

  bool
  assignFromOperands(D::value_type &transfer, unsigned destBit,
                     std::initializer_list<const llvm::Value *> ops) const {
    std::vector<unsigned> bits;
    bool gen = false;
    for (const llvm::Value *Op : ops) {
      if (llvm::isa<llvm::UndefValue>(Op))
        gen = true;
      unsigned bit = info.getValueBit(Op);
      if (bit != UninitializedInfo::invalidBit())
        bits.push_back(bit);
    }
    return assignFromSources(transfer, destBit, bits, gen);
  }

  bool assignFromOperands(D::value_type &transfer, unsigned destBit,
                          const std::vector<const llvm::Value *> &ops) const {
    std::vector<unsigned> bits;
    bool gen = false;
    for (const llvm::Value *Op : ops) {
      if (llvm::isa<llvm::UndefValue>(Op))
        gen = true;
      unsigned bit = info.getValueBit(Op);
      if (bit != UninitializedInfo::invalidBit())
        bits.push_back(bit);
    }
    return assignFromSources(transfer, destBit, bits, gen);
  }
};

} // namespace

InterMaybeUninitialized::Result
InterMaybeUninitialized::run(llvm::Module &M, bool verbose,
                             LinearStrategy linearStrategy,
                             IndirectCallResolutionMode callResolutionMode) {
  MaybeUninitializedAnalysis analysis(M);
  auto engineResult =
      InterEngine<TaintTransformer, MaybeUninitializedAnalysis>::run(
          M, analysis, verbose, linearStrategy, callResolutionMode);

  InterMaybeUninitialized::Result result;
  result.status = engineResult.status;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  return result;
}

} // namespace npa
