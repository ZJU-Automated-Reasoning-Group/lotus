/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/Nullability.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace npa {

namespace {

struct MemKey {
  const llvm::Value *base = nullptr;
  const llvm::Value *ptr = nullptr;
  int64_t offset = 0;
  bool unknown = false;
  bool precise = false;
};

struct MemKeyHash {
  size_t operator()(const MemKey &k) const {
    size_t h = 0;
    h ^= std::hash<const void *>{}(k.base) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(k.offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.unknown) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.precise) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

struct MemKeyEq {
  bool operator()(const MemKey &a, const MemKey &b) const {
    return a.base == b.base && a.offset == b.offset && a.unknown == b.unknown &&
           a.precise == b.precise;
  }
};

class NullabilityInfo {
public:
  struct RelativeOffsetInfo {
    bool derived = false;
    bool unknown = false;
    int64_t offset = 0;
  };

  explicit NullabilityInfo(llvm::Module &M, lotus::AliasAnalysisWrapper &aa)
      : module(M), aliasAnalysis(aa), dataLayout(M.getDataLayout()) {
    buildUniverse();
  }

  unsigned getBitWidth() const { return nextBit == 0 ? 1u : nextBit; }

  unsigned getValueBit(const llvm::Value *V) const {
    auto It = valueBits.find(V);
    if (It == valueBits.end())
      return invalidBit();
    return It->second;
  }

  unsigned getMemoryBit(const llvm::Value *Pointer) const {
    unsigned bit = invalidBit();
    if (getPreciseDirectMemBit(Pointer, bit))
      return bit;
    return invalidBit();
  }

  bool getPreciseDirectMemBit(const llvm::Value *Pointer, unsigned &bit) const {
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return false;

    MemKey key = buildMemKey(Pointer);
    auto It = memoryBits.find(key);
    if (It == memoryBits.end() || !key.precise)
      return false;

    bit = It->second;
    return true;
  }

  std::vector<unsigned>
  getDirectAccessMemBits(const llvm::Value *Pointer,
                         const llvm::Function *FunctionScope = nullptr) const {
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return {};

    unsigned preciseBit = invalidBit();
    if (getPreciseDirectMemBit(Pointer, preciseBit))
      return {preciseBit};
    return getAliasMemBits(Pointer, FunctionScope);
  }

  std::vector<unsigned>
  getMemoryBitsForPointer(const llvm::Value *Pointer,
                          const llvm::Function *FunctionScope = nullptr) const {
    std::vector<unsigned> out = getAliasMemBits(Pointer, FunctionScope);
    unsigned preciseBit = invalidBit();
    if (getPreciseDirectMemBit(Pointer, preciseBit))
      out.push_back(preciseBit);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  std::vector<unsigned>
  getAliasMemBits(const llvm::Value *Pointer,
                  const llvm::Function *FunctionScope = nullptr) const {
    std::vector<unsigned> out;
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return out;

    MemKey key = buildMemKey(Pointer);
    for (const auto &entry : memoryBits) {
      const llvm::Function *owner = getOwningFunction(entry.first.ptr);
      if (FunctionScope && owner && owner != FunctionScope)
        continue;
      if (mayAlias(key, entry.first))
        out.push_back(entry.second);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  const std::unordered_map<const llvm::Value *, unsigned> &
  getValueBits() const {
    return valueBits;
  }

  std::unordered_map<const llvm::Value *, unsigned>
  buildPreciseMemoryBits() const {
    std::unordered_map<const llvm::Value *, unsigned> out;
    for (const auto *Pointer : memoryPointers) {
      unsigned bit = invalidBit();
      if (getPreciseDirectMemBit(Pointer, bit))
        out.emplace(Pointer, bit);
    }
    return out;
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  buildPointerMemoryBits() const {
    std::unordered_map<const llvm::Value *, std::vector<unsigned>> out;
    for (const auto &entry : valueBits) {
      auto bits = getMemoryBitsForPointer(entry.first);
      if (!bits.empty())
        out.emplace(entry.first, std::move(bits));
    }
    return out;
  }

  const std::unordered_set<const llvm::Value *> &getMemoryPointers() const {
    return memoryPointers;
  }

  bool getPreciseSingletonMemBit(const llvm::Value *Pointer,
                                 const llvm::Function *FunctionScope,
                                 unsigned &bit) const {
    if (!getPreciseDirectMemBit(Pointer, bit))
      return false;

    auto bits = getMemoryBitsForPointer(Pointer, FunctionScope);
    return bits.size() == 1 && bits.front() == bit;
  }

  std::vector<unsigned> getReachableMemoryBitsFromRoot(
      const llvm::Value *Root,
      const llvm::Function *FunctionScope = nullptr) const {
    std::vector<unsigned> out = getAliasMemBits(Root, FunctionScope);
    if (Root && Root->getType()->isPointerTy()) {
      for (const auto *candidate : memoryPointers) {
        if (!candidate || !isValueInFunctionScope(candidate, FunctionScope))
          continue;
        auto candidateRelative = getRelativeOffsetInfo(candidate, Root);
        if (!candidateRelative.derived)
          continue;
        unsigned bit = getMemoryBit(candidate);
        if (bit != invalidBit())
          out.push_back(bit);
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  RelativeOffsetInfo getRelativeOffsetInfo(const llvm::Value *Pointer,
                                           const llvm::Value *Root) const {
    std::unordered_set<const llvm::Value *> visited;
    return getRelativeOffsetInfoImpl(Pointer, Root, visited);
  }

  std::vector<unsigned>
  getCallerBitsForRelativeAccess(const llvm::Value *Actual,
                                 const RelativeOffsetInfo &relative,
                                 const llvm::Function *Caller) const {
    std::vector<unsigned> out;
    if (!Actual || !Actual->getType()->isPointerTy())
      return out;

    MemKey actualKey = buildMemKey(Actual);
    if (actualKey.base && !actualKey.unknown && relative.derived &&
        !relative.unknown) {
      const int64_t wantedOffset = actualKey.offset + relative.offset;
      for (const auto *candidate : memoryPointers) {
        if (!candidate)
          continue;
        const llvm::Function *owner = getOwningFunction(candidate);
        if (Caller && owner && owner != Caller)
          continue;

        MemKey candidateKey = buildMemKey(candidate);
        if (candidateKey.base != actualKey.base || candidateKey.unknown ||
            candidateKey.offset != wantedOffset) {
          continue;
        }

        unsigned bit = getMemoryBit(candidate);
        if (bit != invalidBit())
          out.push_back(bit);
      }
      if (!out.empty()) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
      }
    }

    for (const auto *candidate : memoryPointers) {
      if (!candidate)
        continue;
      const llvm::Function *owner = getOwningFunction(candidate);
      if (Caller && owner && owner != Caller)
        continue;

      auto candidateRelative = getRelativeOffsetInfo(candidate, Actual);
      if (!candidateRelative.derived)
        continue;
      if (relative.unknown || candidateRelative.unknown ||
          candidateRelative.offset == relative.offset) {
        unsigned bit = getMemoryBit(candidate);
        if (bit != invalidBit())
          out.push_back(bit);
      }
    }

    if (out.empty() && relative.derived) {
      for (const llvm::Value *root :
           getAliasingRootPointers(Actual, Caller, /*includeActual=*/true)) {
        appendDerivedBitsForRoot(out, root, relative, Caller);
      }
    }

    if (out.empty() && relative.derived &&
        (relative.unknown || relative.offset == 0)) {
      out = getAliasMemBits(Actual, Caller);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  static const llvm::Function *getOwningFunction(const llvm::Value *value) {
    if (auto *Arg = llvm::dyn_cast_or_null<llvm::Argument>(value))
      return Arg->getParent();
    if (auto *Inst = llvm::dyn_cast_or_null<llvm::Instruction>(value))
      return Inst->getFunction();
    return nullptr;
  }

  static unsigned invalidBit() { return static_cast<unsigned>(-1); }

private:
  llvm::Module &module;
  lotus::AliasAnalysisWrapper &aliasAnalysis;
  llvm::DataLayout dataLayout;
  unsigned nextBit = 0;
  std::unordered_map<const llvm::Value *, unsigned> valueBits;
  std::unordered_map<MemKey, unsigned, MemKeyHash, MemKeyEq> memoryBits;
  std::unordered_set<const llvm::Value *> memoryPointers;

  bool isValueInFunctionScope(const llvm::Value *value,
                              const llvm::Function *FunctionScope) const {
    if (!FunctionScope)
      return true;
    const llvm::Function *owner = getOwningFunction(value);
    return owner == nullptr || owner == FunctionScope;
  }

  void appendDerivedBitsForRoot(std::vector<unsigned> &out,
                                const llvm::Value *Root,
                                const RelativeOffsetInfo &relative,
                                const llvm::Function *FunctionScope) const {
    if (!Root || !relative.derived)
      return;

    for (const auto *candidate : memoryPointers) {
      if (!candidate || !isValueInFunctionScope(candidate, FunctionScope))
        continue;

      auto candidateRelative = getRelativeOffsetInfo(candidate, Root);
      if (!candidateRelative.derived)
        continue;
      if (relative.unknown || candidateRelative.unknown ||
          candidateRelative.offset == relative.offset) {
        auto bits = getMemoryBitsForPointer(candidate, FunctionScope);
        out.insert(out.end(), bits.begin(), bits.end());
      }
    }
  }

  std::vector<const llvm::Value *>
  getAliasingRootPointers(const llvm::Value *Pointer,
                          const llvm::Function *FunctionScope,
                          bool includeActual) const {
    std::vector<const llvm::Value *> out;
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return out;

    if (includeActual)
      out.push_back(Pointer);

    for (const auto &entry : valueBits) {
      const llvm::Value *candidate = entry.first;
      if (!candidate || candidate == Pointer ||
          !isValueInFunctionScope(candidate, FunctionScope))
        continue;
      if (!aliasAnalysis.mayAlias(candidate, Pointer))
        continue;
      out.push_back(candidate);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  static bool pointsToPointerStorage(const llvm::Value *Pointer) {
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return false;
    const llvm::Type *Pointee = Pointer->getType()->getPointerElementType();
    return Pointee && Pointee->isPointerTy();
  }

  void addValueBit(const llvm::Value *V) {
    if (!V || !V->getType()->isPointerTy() || valueBits.count(V))
      return;
    valueBits.emplace(V, nextBit++);
  }

  void addMemoryBit(const llvm::Value *Pointer) {
    if (!pointsToPointerStorage(Pointer))
      return;
    memoryPointers.insert(Pointer);
    MemKey key = buildMemKey(Pointer);
    if (memoryBits.count(key))
      return;
    memoryBits.emplace(key, nextBit++);
  }

  bool buildPreciseMemKey(const llvm::Value *Pointer, MemKey &key) const {
    if (!Pointer || !Pointer->getType()->isPointerTy())
      return false;

    const llvm::Value *stripped = Pointer->stripPointerCasts();
    if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(stripped)) {
      if (!buildPreciseMemKey(GEP->getPointerOperand(), key))
        return false;
      llvm::APInt offAP(64, 0, true);
      if (!GEP->accumulateConstantOffset(dataLayout, offAP))
        return false;
      key.offset += offAP.getSExtValue();
      return true;
    }

    if (auto *Root = llvm::dyn_cast<llvm::Argument>(stripped)) {
      key.base = Root;
      key.offset = 0;
      key.unknown = false;
      key.precise = true;
      return true;
    }
    if (auto *Root = llvm::dyn_cast<llvm::GlobalValue>(stripped)) {
      key.base = Root;
      key.offset = 0;
      key.unknown = false;
      key.precise = true;
      return true;
    }
    if (auto *Root = llvm::dyn_cast<llvm::AllocaInst>(stripped)) {
      key.base = Root;
      key.offset = 0;
      key.unknown = false;
      key.precise = true;
      return true;
    }

    return false;
  }

  MemKey buildMemKey(const llvm::Value *Pointer) const {
    MemKey key;
    key.ptr = Pointer;
    if (!Pointer)
      return key;

    if (buildPreciseMemKey(Pointer, key))
      return key;

    const llvm::Value *stripped = Pointer->stripPointerCasts();
    key.base = llvm::getUnderlyingObject(stripped);
    key.offset = 0;
    key.unknown = true;
    key.precise = false;

    if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(stripped)) {
      llvm::APInt offAP(64, 0, true);
      if (GEP->accumulateConstantOffset(dataLayout, offAP))
        key.offset = offAP.getSExtValue();
    }

    return key;
  }

  bool mayAlias(const MemKey &a, const MemKey &b) const {
    if (!a.ptr || !b.ptr)
      return false;
    if (a.base == b.base) {
      if (a.unknown || b.unknown)
        return true;
      return a.offset == b.offset;
    }

    if (a.base && b.base && llvm::isIdentifiedObject(a.base) &&
        llvm::isIdentifiedObject(b.base) && a.base != b.base) {
      return false;
    }

    return aliasAnalysis.mayAlias(a.ptr, b.ptr);
  }

  RelativeOffsetInfo getRelativeOffsetInfoImpl(
      const llvm::Value *Pointer, const llvm::Value *Root,
      std::unordered_set<const llvm::Value *> &visited) const {
    RelativeOffsetInfo out;
    if (!Pointer || !Root)
      return out;
    if (!visited.insert(Pointer).second)
      return out;

    const llvm::Value *stripped = Pointer->stripPointerCasts();
    if (Pointer == Root || stripped == Root) {
      out.derived = true;
      return out;
    }

    if (auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(Pointer)) {
      out = getRelativeOffsetInfoImpl(GEP->getPointerOperand(), Root, visited);
      if (!out.derived || out.unknown)
        return out;
      llvm::APInt offAP(64, 0, true);
      if (GEP->accumulateConstantOffset(dataLayout, offAP))
        out.offset += offAP.getSExtValue();
      else
        out.unknown = true;
      return out;
    }

    if (auto *Op = llvm::dyn_cast<llvm::Operator>(Pointer)) {
      if (Op->getOpcode() == llvm::Instruction::BitCast ||
          Op->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        return getRelativeOffsetInfoImpl(Op->getOperand(0), Root, visited);
      }
      if (Op->getOpcode() == llvm::Instruction::Select) {
        auto T = getRelativeOffsetInfoImpl(Op->getOperand(1), Root, visited);
        auto F = getRelativeOffsetInfoImpl(Op->getOperand(2), Root, visited);
        if (!T.derived && !F.derived)
          return {};
        if (T.derived && F.derived && !T.unknown && !F.unknown &&
            T.offset == F.offset) {
          return T;
        }
        out.derived = T.derived || F.derived;
        out.unknown = out.derived;
        return out;
      }
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Pointer)) {
      bool saw = false;
      int64_t commonOffset = 0;
      bool commonKnown = true;
      for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
        auto incoming =
            getRelativeOffsetInfoImpl(Phi->getIncomingValue(i), Root, visited);
        if (!incoming.derived)
          continue;
        if (!saw) {
          saw = true;
          commonOffset = incoming.offset;
          commonKnown = !incoming.unknown;
        } else if (incoming.unknown || !commonKnown ||
                   incoming.offset != commonOffset) {
          commonKnown = false;
        }
      }
      if (saw) {
        out.derived = true;
        out.unknown = !commonKnown;
        out.offset = commonOffset;
      }
    }
    return out;
  }

  void buildUniverse() {
    for (auto &G : module.globals()) {
      addValueBit(&G);
      addMemoryBit(&G);
    }

    for (auto &F : module) {
      if (F.isDeclaration())
        continue;
      for (auto &Arg : F.args()) {
        addValueBit(&Arg);
        addMemoryBit(&Arg);
      }
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (I.getType()->isPointerTy()) {
            addValueBit(&I);
            addMemoryBit(&I);
          }
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            addMemoryBit(Load->getPointerOperand());
          } else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            addMemoryBit(Store->getPointerOperand());
          } else if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            for (llvm::Use &ArgUse : Call->args())
              addMemoryBit(ArgUse.get());
          }
        }
      }
    }
  }
};

class NullabilityAnalysis {
public:
  using FactType = llvm::APInt;
  using D = TaintTransformer;
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Engine = InterEngine<D, NullabilityAnalysis>;

  enum class NullKind {
    KnownNull,
    KnownNonNull,
    Unknown,
  };

  NullabilityAnalysis(llvm::Module &M, lotus::AliasAnalysisWrapper &aa,
                      const InterNullability::Options &opts)
      : module(M), info(M, aa), options(opts), bitWidth(info.getBitWidth()),
        widthScope(bitWidth), entryFacts(bitWidth, 0) {
    initializeEntryFacts();
  }

  FactType getEntryValue() const { return entryFacts; }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I))
      return currentPath;

    D::value_type transfer = D::one();
    if (!buildNormalTransfer(I, transfer))
      return currentPath;
    return Exp::seq(transfer, currentPath);
  }

  bool buildNormalTransfer(llvm::Instruction &I,
                           D::value_type &transfer) const {
    transfer = D::one();
    bool updated = false;

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
      updated |= assignNonNull(transfer, info.getValueBit(Alloca));
    } else if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      if (Store->getValueOperand()->getType()->isPointerTy()) {
        auto targetBits = info.getMemoryBitsForPointer(
            Store->getPointerOperand(), I.getFunction());
        auto source = sourceFromValue(Store->getValueOperand());
        unsigned preciseBit = NullabilityInfo::invalidBit();
        if (targetBits.size() == 1 &&
            info.getPreciseSingletonMemBit(Store->getPointerOperand(),
                                           I.getFunction(), preciseBit)) {
          updated |=
              assignFromSources(transfer, preciseBit, source.bits, source.gen);
        } else {
          for (unsigned targetBit : targetBits) {
            updated |= assignFromSourcesWeak(transfer, targetBit, source.bits,
                                             source.gen);
          }
        }
      }
    } else if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      if (Load->getType()->isPointerTy()) {
        auto srcBits = info.getMemoryBitsForPointer(Load->getPointerOperand(),
                                                    I.getFunction());
        updated |= assignFromSources(transfer, info.getValueBit(Load), srcBits,
                                     srcBits.empty());
      }
    } else if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      if (Cast->getType()->isPointerTy()) {
        updated |= assignFromOperands(transfer, info.getValueBit(Cast),
                                      {Cast->getOperand(0)});
      }
    } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
      updated |= assignFromOperands(transfer, info.getValueBit(GEP),
                                    {GEP->getPointerOperand()});
    } else if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      if (Select->getType()->isPointerTy()) {
        updated |= assignFromOperands(
            transfer, info.getValueBit(Select),
            {Select->getTrueValue(), Select->getFalseValue()});
      }
    } else if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(&I)) {
      if (Phi->getType()->isPointerTy()) {
        std::vector<const llvm::Value *> incoming;
        incoming.reserve(Phi->getNumIncomingValues());
        for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i)
          incoming.push_back(Phi->getIncomingValue(i));
        updated |=
            assignFromOperands(transfer, info.getValueBit(Phi), incoming);
      }
    } else if (I.getType()->isPointerTy()) {
      updated |= assignUnknown(transfer, info.getValueBit(&I));
    }

    return updated;
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &Call,
                                     const llvm::Function &Callee) const {
    D::value_type transfer = D::one();
    bool updated = false;
    const size_t count = std::min<size_t>(
        Call.arg_size(), static_cast<size_t>(Callee.arg_size()));
    const auto *ParamIt = Callee.arg_begin();
    for (size_t i = 0; i < count; ++i, ++ParamIt) {
      const llvm::Value *Arg = Call.getArgOperand(i);
      if (!Arg->getType()->isPointerTy() || !ParamIt->getType()->isPointerTy())
        continue;

      updated |= assignFromValue(transfer, info.getValueBit(&*ParamIt), Arg);
      updated |=
          seedFormalMemoryFromActual(transfer, Call, Callee, &*ParamIt, Arg);
    }
    return updated ? transfer : D::one();
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &Call,
                                      const llvm::Function &Callee) const {
    if (Callee.isDeclaration())
      return getConservativeExternalTransfer(Call);

    D::value_type transfer = D::one();
    bool updated = false;

    if (Call.getType()->isPointerTy()) {
      std::vector<unsigned> retBits;
      bool retMayBeNull = false;
      for (const auto &BB : Callee) {
        auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
        if (!Ret)
          continue;
        const llvm::Value *RetVal = Ret->getReturnValue();
        if (!RetVal || !RetVal->getType()->isPointerTy())
          continue;
        unsigned retBit = info.getValueBit(RetVal);
        if (retBit != NullabilityInfo::invalidBit())
          retBits.push_back(retBit);
        NullKind kind = classifyPointerValue(RetVal);
        if (kind == NullKind::KnownNull ||
            (kind == NullKind::Unknown &&
             retBit == NullabilityInfo::invalidBit())) {
          retMayBeNull = true;
        }
      }
      updated |= assignFromSources(transfer, info.getValueBit(&Call), retBits,
                                   retMayBeNull);
    }

    const size_t count = std::min<size_t>(
        Call.arg_size(), static_cast<size_t>(Callee.arg_size()));
    const auto *ParamIt = Callee.arg_begin();
    for (size_t i = 0; i < count; ++i, ++ParamIt) {
      const llvm::Value *Arg = Call.getArgOperand(i);
      if (!Arg->getType()->isPointerTy() || !ParamIt->getType()->isPointerTy())
        continue;
      updated |=
          projectFormalMemoryToActual(transfer, Call, Callee, &*ParamIt, Arg);
    }

    return updated ? transfer : D::one();
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &Call) const {
    if (!options.treat_unknown_calls_as_maybe_null)
      return D::one();
    return getConservativeExternalTransfer(Call);
  }

  D::value_type
  getCallFallbackTransfer(const llvm::CallBase &Call,
                          const std::vector<llvm::Function *> &Resolved) const {
    if (!options.treat_unknown_calls_as_maybe_null)
      return D::zero();

    bool hasUnknownOrExternalTarget =
        Resolved.empty() && Call.getCalledFunction() == nullptr;
    for (const auto *Callee : Resolved) {
      if (!Callee || Callee->isDeclaration()) {
        hasUnknownOrExternalTarget = true;
        break;
      }
    }
    if (!hasUnknownOrExternalTarget)
      return D::zero();
    return getConservativeExternalTransfer(Call);
  }

  D::value_type buildCallTransfer(
      const llvm::CallBase &Call,
      const std::map<const llvm::Function *, D::value_type> &summaryMap) const {
    std::vector<llvm::Function *> Callees =
        Engine::getPossibleCallees(module, Call);
    D::value_type combined = D::zero();
    bool hasBranch = false;

    for (llvm::Function *Callee : Callees) {
      D::value_type branch = D::zero();
      if (!Callee || Callee->isDeclaration()) {
        branch = getCallReturnTransfer(Call, *Callee);
      } else {
        auto SummaryIt = summaryMap.find(Callee);
        if (SummaryIt == summaryMap.end()) {
          branch = getConservativeExternalTransfer(Call);
        } else {
          branch = D::extend(getCallReturnTransfer(Call, *Callee),
                             D::extend(SummaryIt->second,
                                       getCallEntryTransfer(Call, *Callee)));
        }
      }

      combined = hasBranch ? D::combine(combined, branch) : branch;
      hasBranch = true;
    }

    D::value_type fallback = getCallFallbackTransfer(Call, Callees);
    if (!D::equal(fallback, D::zero())) {
      combined = hasBranch ? D::combine(combined, fallback) : fallback;
      hasBranch = true;
    }

    if (!hasBranch)
      return getCallToReturnTransfer(Call);
    return combined;
  }

  FactType applySummary(const D::value_type &summary,
                        const FactType &fact) const {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return lhs | rhs;
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return lhs == rhs;
  }

  const std::unordered_map<const llvm::Value *, unsigned> &
  getValueBits() const {
    return info.getValueBits();
  }

  std::unordered_map<const llvm::Value *, unsigned> getMemoryBits() const {
    return info.buildPreciseMemoryBits();
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  getPointerMemoryBits() const {
    return info.buildPointerMemoryBits();
  }

private:
  llvm::Module &module;
  NullabilityInfo info;
  InterNullability::Options options;
  unsigned bitWidth = 1;
  D::WidthScope widthScope;
  FactType entryFacts;

  struct SourceInfo {
    std::vector<unsigned> bits;
    bool gen = false;
  };

  static NullKind classifyPointerValue(const llvm::Value *V) {
    if (!V || !V->getType()->isPointerTy())
      return NullKind::KnownNonNull;
    const llvm::Value *Stripped = V->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(Stripped))
      return NullKind::KnownNull;
    if (llvm::isa<llvm::UndefValue>(Stripped) ||
        llvm::isa<llvm::PoisonValue>(Stripped))
      return NullKind::Unknown;
    if (llvm::isa<llvm::Function>(Stripped) ||
        llvm::isa<llvm::GlobalValue>(Stripped) ||
        llvm::isa<llvm::AllocaInst>(Stripped))
      return NullKind::KnownNonNull;
    if (auto *C = llvm::dyn_cast<llvm::Constant>(Stripped))
      return C->isNullValue() ? NullKind::KnownNull : NullKind::KnownNonNull;
    return NullKind::Unknown;
  }

  static llvm::Constant *
  getAggregateElementConstant(const llvm::Constant *Aggregate, unsigned Index,
                              llvm::Type *ElementTy) {
    if (!Aggregate)
      return nullptr;
    if (llvm::isa<llvm::PoisonValue>(Aggregate))
      return llvm::PoisonValue::get(ElementTy);
    if (llvm::isa<llvm::UndefValue>(Aggregate))
      return llvm::UndefValue::get(ElementTy);
    if (Aggregate->isNullValue())
      return llvm::Constant::getNullValue(ElementTy);
    if (auto *Element = Aggregate->getAggregateElement(Index))
      return Element;
    return nullptr;
  }

  void collectPointerInitializerKinds(
      const llvm::Constant *Init, llvm::Type *Ty, int64_t baseOffset,
      std::unordered_map<int64_t, NullKind> &out) const {
    if (!Ty)
      return;
    if (Ty->isPointerTy()) {
      const llvm::Value *Value = Init ? static_cast<const llvm::Value *>(Init)
                                      : static_cast<const llvm::Value *>(
                                            llvm::Constant::getNullValue(Ty));
      out[baseOffset] = classifyPointerValue(Value);
      return;
    }

    if (auto *StructTy = llvm::dyn_cast<llvm::StructType>(Ty)) {
      const llvm::StructLayout *Layout =
          module.getDataLayout().getStructLayout(StructTy);
      for (unsigned i = 0; i < StructTy->getNumElements(); ++i) {
        llvm::Type *ElementTy = StructTy->getElementType(i);
        collectPointerInitializerKinds(
            getAggregateElementConstant(Init, i, ElementTy), ElementTy,
            baseOffset + static_cast<int64_t>(Layout->getElementOffset(i)),
            out);
      }
      return;
    }

    if (auto *ArrayTy = llvm::dyn_cast<llvm::ArrayType>(Ty)) {
      llvm::Type *ElementTy = ArrayTy->getElementType();
      const uint64_t elementSize =
          module.getDataLayout().getTypeAllocSize(ElementTy);
      for (uint64_t i = 0; i < ArrayTy->getNumElements(); ++i) {
        collectPointerInitializerKinds(
            getAggregateElementConstant(Init, static_cast<unsigned>(i),
                                        ElementTy),
            ElementTy, baseOffset + static_cast<int64_t>(i * elementSize), out);
      }
      return;
    }
  }

  void seedDerivedMemoryFromRoot(const llvm::Value *Root,
                                 const llvm::Function *Owner = nullptr) {
    for (const auto *Pointer : info.getMemoryPointers()) {
      const llvm::Function *PointerOwner =
          NullabilityInfo::getOwningFunction(Pointer);
      if (Owner && PointerOwner != Owner)
        continue;
      auto relative = info.getRelativeOffsetInfo(Pointer, Root);
      if (!relative.derived)
        continue;
      unsigned bit = info.getMemoryBit(Pointer);
      if (bit != NullabilityInfo::invalidBit())
        entryFacts.setBit(bit);
    }
  }

  void initializeEntryFacts() {
    if (options.seed_entry_pointer_args) {
      for (llvm::Function *Entry : Engine::getEntryFunctions(module)) {
        if (!Entry)
          continue;
        for (auto &Arg : Entry->args()) {
          if (!Arg.getType()->isPointerTy())
            continue;
          unsigned valueBit = info.getValueBit(&Arg);
          if (valueBit != NullabilityInfo::invalidBit())
            entryFacts.setBit(valueBit);
          seedDerivedMemoryFromRoot(&Arg, Entry);
        }
      }
    }

    for (auto &Global : module.globals()) {
      std::unordered_map<int64_t, NullKind> initializerKinds;
      if (Global.hasInitializer()) {
        collectPointerInitializerKinds(Global.getInitializer(),
                                       Global.getValueType(), 0,
                                       initializerKinds);
      }

      for (const auto *Pointer : info.getMemoryPointers()) {
        auto relative = info.getRelativeOffsetInfo(Pointer, &Global);
        if (!relative.derived)
          continue;
        unsigned memBit = info.getMemoryBit(Pointer);
        if (memBit == NullabilityInfo::invalidBit())
          continue;

        if (Global.isDeclaration() || relative.unknown) {
          entryFacts.setBit(memBit);
          continue;
        }

        auto kindIt = initializerKinds.find(relative.offset);
        if (kindIt == initializerKinds.end() ||
            kindIt->second != NullKind::KnownNonNull) {
          entryFacts.setBit(memBit);
        }
      }
    }
  }

  void clearDestination(D::value_type &transfer, unsigned destBit) const {
    if (destBit == NullabilityInfo::invalidBit())
      return;
    for (auto &row : transfer.rel)
      row.clearBit(destBit);
    transfer.gen.clearBit(destBit);
  }

  bool assignFromSources(D::value_type &transfer, unsigned destBit,
                         const std::vector<unsigned> &sourceBits,
                         bool gen) const {
    if (destBit == NullabilityInfo::invalidBit())
      return false;
    clearDestination(transfer, destBit);
    for (unsigned srcBit : sourceBits) {
      if (srcBit != NullabilityInfo::invalidBit())
        D::addEdge(transfer, srcBit, destBit);
    }
    if (gen)
      D::addGen(transfer, destBit);
    return true;
  }

  bool assignFromSourcesWeak(D::value_type &transfer, unsigned destBit,
                             const std::vector<unsigned> &sourceBits,
                             bool gen) const {
    if (destBit == NullabilityInfo::invalidBit())
      return false;
    bool changed = false;
    for (unsigned srcBit : sourceBits) {
      if (srcBit == NullabilityInfo::invalidBit())
        continue;
      D::addEdge(transfer, srcBit, destBit);
      changed = true;
    }
    if (gen) {
      D::addGen(transfer, destBit);
      changed = true;
    }
    return changed;
  }

  SourceInfo sourceFromValue(const llvm::Value *V) const {
    SourceInfo source;
    if (!V || !V->getType()->isPointerTy()) {
      source.gen = true;
      return source;
    }
    unsigned srcBit = info.getValueBit(V);
    if (srcBit != NullabilityInfo::invalidBit())
      source.bits.push_back(srcBit);

    NullKind kind = classifyPointerValue(V);
    source.gen = kind == NullKind::KnownNull;
    if (kind == NullKind::Unknown && srcBit == NullabilityInfo::invalidBit())
      source.gen = true;
    return source;
  }

  bool assignFromValue(D::value_type &transfer, unsigned destBit,
                       const llvm::Value *V) const {
    if (destBit == NullabilityInfo::invalidBit())
      return false;
    SourceInfo source = sourceFromValue(V);
    return assignFromSources(transfer, destBit, source.bits, source.gen);
  }

  bool
  assignFromOperands(D::value_type &transfer, unsigned destBit,
                     std::initializer_list<const llvm::Value *> ops) const {
    std::vector<const llvm::Value *> values(ops.begin(), ops.end());
    return assignFromOperands(transfer, destBit, values);
  }

  bool assignFromOperands(D::value_type &transfer, unsigned destBit,
                          const std::vector<const llvm::Value *> &ops) const {
    if (destBit == NullabilityInfo::invalidBit())
      return false;

    std::vector<unsigned> bits;
    bool gen = false;
    for (const llvm::Value *Op : ops) {
      if (!Op || !Op->getType()->isPointerTy())
        continue;
      unsigned bit = info.getValueBit(Op);
      if (bit != NullabilityInfo::invalidBit())
        bits.push_back(bit);

      NullKind kind = classifyPointerValue(Op);
      if (kind == NullKind::KnownNull)
        gen = true;
      else if (kind == NullKind::Unknown &&
               bit == NullabilityInfo::invalidBit())
        gen = true;
    }

    std::sort(bits.begin(), bits.end());
    bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    return assignFromSources(transfer, destBit, bits, gen);
  }

  bool assignNonNull(D::value_type &transfer, unsigned destBit) const {
    return assignFromSources(transfer, destBit, {}, false);
  }

  bool assignUnknown(D::value_type &transfer, unsigned destBit) const {
    return assignFromSources(transfer, destBit, {}, true);
  }

  bool seedFormalMemoryFromActual(D::value_type &transfer,
                                  const llvm::CallBase &Call,
                                  const llvm::Function &Callee,
                                  const llvm::Value *Formal,
                                  const llvm::Value *Actual) const {
    const llvm::Function *Caller = Call.getFunction();
    std::unordered_map<unsigned, SourceInfo> pending;
    for (const auto *Pointer : info.getMemoryPointers()) {
      if (NullabilityInfo::getOwningFunction(Pointer) != &Callee)
        continue;
      auto relative = info.getRelativeOffsetInfo(Pointer, Formal);
      if (!relative.derived)
        continue;

      auto destBits = info.getMemoryBitsForPointer(Pointer, &Callee);
      if (destBits.empty())
        continue;

      auto srcBits =
          info.getCallerBitsForRelativeAccess(Actual, relative, Caller);
      for (unsigned destBit : destBits) {
        auto &entry = pending[destBit];
        entry.bits.insert(entry.bits.end(), srcBits.begin(), srcBits.end());
        entry.gen = entry.gen || srcBits.empty();
      }
    }

    bool updated = false;
    for (auto &entry : pending) {
      auto &bits = entry.second.bits;
      std::sort(bits.begin(), bits.end());
      bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
      updated |=
          assignFromSources(transfer, entry.first, bits, entry.second.gen);
    }
    return updated;
  }

  bool projectFormalMemoryToActual(D::value_type &transfer,
                                   const llvm::CallBase &Call,
                                   const llvm::Function &Callee,
                                   const llvm::Value *Formal,
                                   const llvm::Value *Actual) const {
    struct ProjectionInfo {
      std::vector<unsigned> srcBits;
      bool weak = false;
    };

    const llvm::Function *Caller = Call.getFunction();
    std::unordered_map<unsigned, ProjectionInfo> pending;
    for (const auto *Pointer : info.getMemoryPointers()) {
      if (NullabilityInfo::getOwningFunction(Pointer) != &Callee)
        continue;
      auto relative = info.getRelativeOffsetInfo(Pointer, Formal);
      if (!relative.derived)
        continue;

      auto srcBits = info.getMemoryBitsForPointer(Pointer, &Callee);
      if (srcBits.empty())
        continue;

      auto dstBits =
          info.getCallerBitsForRelativeAccess(Actual, relative, Caller);
      if (dstBits.empty())
        continue;

      unsigned preciseSrcBit = NullabilityInfo::invalidBit();
      const bool preciseSource =
          info.getPreciseSingletonMemBit(Pointer, &Callee, preciseSrcBit);
      const bool weakUpdate = dstBits.size() != 1 || !preciseSource;

      for (unsigned dstBit : dstBits) {
        auto &entry = pending[dstBit];
        entry.srcBits.insert(entry.srcBits.end(), srcBits.begin(),
                             srcBits.end());
        entry.weak = entry.weak || weakUpdate;
      }
    }

    bool updated = false;
    for (auto &entry : pending) {
      auto &srcBits = entry.second.srcBits;
      std::sort(srcBits.begin(), srcBits.end());
      srcBits.erase(std::unique(srcBits.begin(), srcBits.end()), srcBits.end());
      if (entry.second.weak) {
        updated |= assignFromSourcesWeak(transfer, entry.first, srcBits, false);
      } else {
        updated |= assignFromSources(transfer, entry.first, srcBits, false);
      }
    }

    return updated;
  }

  D::value_type
  getConservativeExternalTransfer(const llvm::CallBase &Call) const {
    D::value_type transfer = D::one();
    if (Call.getType()->isPointerTy())
      assignUnknown(transfer, info.getValueBit(&Call));
    for (const llvm::Use &ArgUse : Call.args()) {
      const llvm::Value *Arg = ArgUse.get();
      if (!Arg || !Arg->getType()->isPointerTy())
        continue;
      for (unsigned memBit :
           info.getReachableMemoryBitsFromRoot(Arg, Call.getFunction()))
        assignUnknown(transfer, memBit);
    }
    return transfer;
  }
};

} // namespace

static const llvm::APInt *
findFactForBlock(const InterNullability::Result &result,
                 const llvm::BasicBlock *block) {
  auto exitIt = result.blockExitFacts.find(BlockKey{block});
  if (exitIt != result.blockExitFacts.end())
    return &exitIt->second;
  auto entryIt = result.blockFacts.find(BlockKey{block});
  if (entryIt == result.blockFacts.end())
    return nullptr;
  return &entryIt->second;
}

bool InterNullability::Result::isMaybeNull(const llvm::BasicBlock *block,
                                           const llvm::Value *value) const {
  auto bitIt = valueBits.find(value);
  if (bitIt == valueBits.end())
    return false;
  const llvm::APInt *fact = findFactForBlock(*this, block);
  return fact && bitIt->second < fact->getBitWidth() && (*fact)[bitIt->second];
}

bool InterNullability::Result::isMaybeNullMemory(
    const llvm::BasicBlock *block, const llvm::Value *pointer) const {
  const llvm::APInt *fact = findFactForBlock(*this, block);
  if (!fact)
    return false;

  auto pointerBitsIt = pointerMemoryBits.find(pointer);
  if (pointerBitsIt != pointerMemoryBits.end()) {
    for (unsigned bit : pointerBitsIt->second) {
      if (bit < fact->getBitWidth() && (*fact)[bit])
        return true;
    }
    return false;
  }

  auto bitIt = memoryBits.find(pointer);
  return bitIt != memoryBits.end() && bitIt->second < fact->getBitWidth() &&
         (*fact)[bitIt->second];
}

InterNullability::Result InterNullability::run(
    llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
    const Options &options, bool verbose, LinearStrategy linearStrategy) {
  NullabilityAnalysis analysis(M, aliasAnalysis, options);
  auto engineResult =
      InterEngine<TaintTransformer, NullabilityAnalysis>::run(
          M, analysis, verbose, linearStrategy, options.call_resolution_mode);

  InterNullability::Result result;
  result.status = engineResult.status;
  result.summaries.insert(engineResult.summaries.begin(),
                          engineResult.summaries.end());
  result.blockFacts.insert(engineResult.blockEntryFacts.begin(),
                           engineResult.blockEntryFacts.end());
  result.valueBits = analysis.getValueBits();
  result.memoryBits = analysis.getMemoryBits();
  result.pointerMemoryBits = analysis.getPointerMemoryBits();

  std::map<const llvm::Function *, TaintTransformer::value_type> summaryMap;
  for (const auto &entry : engineResult.summaries)
    summaryMap.emplace(entry.first.function, entry.second);

  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      auto factIt = engineResult.blockEntryFacts.find(BlockKey{&BB});
      llvm::APInt currentFact =
          (&BB == &F.getEntryBlock())
              ? analysis.getEntryValue()
              : llvm::APInt(analysis.getEntryValue().getBitWidth(), 0);
      if (factIt != engineResult.blockEntryFacts.end())
        currentFact = factIt->second;
      else if (&BB != &F.getEntryBlock())
        continue;

      for (auto &I : BB) {
        if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          auto transfer = analysis.buildCallTransfer(*Call, summaryMap);
          currentFact = TaintTransformer::apply(transfer, currentFact);
          continue;
        }

        TaintTransformer::value_type transfer = TaintTransformer::one();
        if (!analysis.buildNormalTransfer(I, transfer))
          continue;
        currentFact = TaintTransformer::apply(transfer, currentFact);
      }
      result.blockExitFacts[{&BB}] = currentFact;
    }
  }

  return result;
}

InterNullability::Result
InterNullability::run(llvm::Module &M,
                      lotus::AliasAnalysisWrapper &aliasAnalysis, bool verbose,
                      LinearStrategy linearStrategy,
                      IndirectCallResolutionMode callResolutionMode) {
  Options options;
  options.call_resolution_mode = callResolutionMode;
  return run(M, aliasAnalysis, options, verbose, linearStrategy);
}

InterNullability::Result InterNullability::run(llvm::Module &M,
                                               const Options &options,
                                               bool verbose,
                                               LinearStrategy linearStrategy) {
  lotus::AliasAnalysisWrapper aliasAnalysis(M, lotus::AAConfig::BasicAA());
  return run(M, aliasAnalysis, options, verbose, linearStrategy);
}

InterNullability::Result
InterNullability::run(llvm::Module &M, bool verbose,
                      LinearStrategy linearStrategy,
                      IndirectCallResolutionMode callResolutionMode) {
  Options options;
  options.call_resolution_mode = callResolutionMode;
  return run(M, options, verbose, linearStrategy);
}

} // namespace npa
