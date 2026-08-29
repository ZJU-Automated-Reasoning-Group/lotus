/*
 * Taint analysis based on the NPA framework
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Inter/Taint.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Annotation/Taint/TaintConfigManager.h"
#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace npa {

struct MemKey {
  const llvm::Value *base = nullptr;
  const llvm::Value *ptr = nullptr;
  int64_t offset = 0;
  bool unknown = false;
};

struct MemKeyHash {
  size_t operator()(const MemKey &k) const {
    size_t h = 0;
    h ^= std::hash<const void *>{}(k.base) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(k.offset) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<bool>{}(k.unknown) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

struct MemKeyEq {
  bool operator()(const MemKey &a, const MemKey &b) const {
    return a.base == b.base && a.offset == b.offset && a.unknown == b.unknown;
  }
};

class TaintInfo {
public:
  struct RelativeOffsetInfo {
    bool derived = false;
    bool unknown = false;
    int64_t offset = 0;
  };

  TaintInfo(llvm::Module &M, lotus::AliasAnalysisWrapper &aa)
      : module(M), aliasAnalysis(aa), dataLayout(M.getDataLayout()) {
    buildUniverse();
    buildReachabilitySeeds();
  }

  unsigned getBitWidth() const { return nextBit; }

  bool hasValueBit(const llvm::Value *v) const {
    return valueBits.find(v) != valueBits.end();
  }

  unsigned getValueBit(const llvm::Value *v) const {
    auto it = valueBits.find(v);
    if (it == valueBits.end())
      return invalidBit();
    return it->second;
  }

  unsigned getMemBitForPtr(const llvm::Value *ptr) const {
    MemKey key = buildMemKey(ptr);
    auto it = memBits.find(key);
    if (it == memBits.end())
      return invalidBit();
    return it->second;
  }

  bool getPreciseDirectMemBit(const llvm::Value *ptr, unsigned &bit) const {
    if (!ptr || !ptr->getType()->isPointerTy())
      return false;

    MemKey key = buildMemKey(ptr);
    auto it = memBits.find(key);
    bool stableDirectKey =
        !key.unknown && key.base &&
        (key.base != ptr || llvm::isa<llvm::Argument>(ptr) ||
         llvm::isa<llvm::GlobalValue>(ptr) || llvm::isa<llvm::AllocaInst>(ptr));
    if (it == memBits.end() || !stableDirectKey)
      return false;

    bit = it->second;
    return true;
  }

  std::vector<unsigned> getDirectAccessMemBits(const llvm::Value *ptr) const {
    if (!ptr || !ptr->getType()->isPointerTy())
      return {};

    unsigned preciseBit = invalidBit();
    if (getPreciseDirectMemBit(ptr, preciseBit))
      return {preciseBit};
    return getAliasMemBits(ptr);
  }

  std::vector<unsigned> getAliasMemBits(const llvm::Value *ptr) const {
    std::vector<unsigned> out;
    if (!ptr || !ptr->getType()->isPointerTy())
      return out;

    MemKey key = buildMemKey(ptr);
    for (const auto &entry : memBits) {
      const MemKey &cand = entry.first;
      if (mayAlias(key, cand))
        out.push_back(entry.second);
    }
    return out;
  }

  std::vector<unsigned>
  getMemoryBitsForAccess(const llvm::Value *ptr,
                         TaintSpec::AccessMode mode) const {
    if (mode == TaintSpec::VALUE || !ptr || !ptr->getType()->isPointerTy())
      return {};
    if (mode == TaintSpec::DIRECT_DEREF)
      return getDirectAccessMemBits(ptr);
    return getReachableMemBits(ptr);
  }

  const llvm::DataLayout &getDataLayout() const { return dataLayout; }

  static unsigned invalidBit() { return static_cast<unsigned>(-1); }

  const std::unordered_map<const llvm::Value *, unsigned> &
  getValueBits() const {
    return valueBits;
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  buildPointerMemoryBits() const {
    std::unordered_map<const llvm::Value *, std::vector<unsigned>> out;
    for (const auto *ptr : memoryPointers) {
      if (!ptr)
        continue;
      auto &bits = out[ptr];
      bits = getDirectAccessMemBits(ptr);
      std::sort(bits.begin(), bits.end());
      bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    }
    return out;
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  buildReachablePointerMemoryBits() const {
    std::unordered_map<const llvm::Value *, std::vector<unsigned>> out;
    for (const auto *ptr : memoryPointers) {
      if (!ptr)
        continue;
      auto &bits = out[ptr];
      bits = getReachableMemBits(ptr);
      std::sort(bits.begin(), bits.end());
      bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    }
    return out;
  }

  const std::unordered_set<const llvm::Value *> &getMemoryPointers() const {
    return memoryPointers;
  }

  const std::vector<const llvm::Value *> *
  findStaticReachableSeeds(unsigned memBit) const {
    auto it = reachablePointerSeeds.find(memBit);
    return it == reachablePointerSeeds.end() ? nullptr : &it->second;
  }

  const std::vector<const llvm::Value *> *
  findPointersForBase(const llvm::Value *base) const {
    auto it = baseToPointers.find(base);
    return it == baseToPointers.end() ? nullptr : &it->second;
  }

  static const llvm::Function *getOwningFunction(const llvm::Value *value) {
    if (auto *arg = llvm::dyn_cast_or_null<llvm::Argument>(value))
      return arg->getParent();
    if (auto *inst = llvm::dyn_cast_or_null<llvm::Instruction>(value))
      return inst->getFunction();
    return nullptr;
  }

  RelativeOffsetInfo getRelativeOffsetInfo(const llvm::Value *ptr,
                                           const llvm::Value *root) const {
    std::unordered_set<const llvm::Value *> visited;
    return getRelativeOffsetInfoImpl(ptr, root, visited);
  }

  std::vector<unsigned>
  getCallerBitsForRelativeAccess(const llvm::Value *actual,
                                 const RelativeOffsetInfo &relative,
                                 const llvm::Function *caller) const {
    std::vector<unsigned> out;
    if (!actual || !actual->getType()->isPointerTy())
      return out;

    MemKey actualKey = buildMemKey(actual);
    if (actualKey.base && !actualKey.unknown && relative.derived &&
        !relative.unknown) {
      const int64_t wantedOffset = actualKey.offset + relative.offset;
      for (const auto *candidate : memoryPointers) {
        if (!candidate)
          continue;
        const llvm::Function *owner = getOwningFunction(candidate);
        if (caller && owner && owner != caller)
          continue;

        MemKey candidateKey = buildMemKey(candidate);
        if (candidateKey.base != actualKey.base || candidateKey.unknown ||
            candidateKey.offset != wantedOffset) {
          continue;
        }

        unsigned bit = getMemBitForPtr(candidate);
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
      if (caller && owner && owner != caller)
        continue;

      auto candidateRelative = getRelativeOffsetInfo(candidate, actual);
      if (!candidateRelative.derived)
        continue;
      if (relative.unknown || candidateRelative.unknown ||
          candidateRelative.offset == relative.offset) {
        unsigned bit = getMemBitForPtr(candidate);
        if (bit != invalidBit())
          out.push_back(bit);
      }
    }

    if (out.empty() && relative.derived &&
        (relative.unknown || relative.offset == 0)) {
      out = relative.unknown ? getReachableMemBits(actual)
                             : getAliasMemBits(actual);
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

private:
  llvm::Module &module;
  lotus::AliasAnalysisWrapper &aliasAnalysis;
  llvm::DataLayout dataLayout;
  unsigned nextBit = 0;
  std::unordered_map<const llvm::Value *, unsigned> valueBits;
  std::unordered_map<MemKey, unsigned, MemKeyHash, MemKeyEq> memBits;
  std::unordered_set<const llvm::Value *> memoryPointers;
  std::unordered_map<unsigned, std::vector<const llvm::Value *>>
      reachablePointerSeeds;
  std::unordered_map<const llvm::Value *, std::vector<const llvm::Value *>>
      baseToPointers;

  void addValueBit(const llvm::Value *v) {
    if (!v)
      return;
    if (valueBits.count(v))
      return;
    valueBits.emplace(v, nextBit++);
  }

  void addMemBit(const llvm::Value *ptr) {
    if (!ptr || !ptr->getType()->isPointerTy())
      return;
    memoryPointers.insert(ptr);
    MemKey key = buildMemKey(ptr);
    if (key.base) {
      auto &pointers = baseToPointers[key.base];
      if (std::find(pointers.begin(), pointers.end(), ptr) == pointers.end())
        pointers.push_back(ptr);
    }
    if (memBits.count(key))
      return;
    memBits.emplace(key, nextBit++);
  }

  MemKey buildMemKey(const llvm::Value *ptr) const {
    MemKey key;
    key.ptr = ptr;
    if (!ptr)
      return key;

    int64_t constantOffset = 0;
    if (const llvm::Value *base = llvm::GetPointerBaseWithConstantOffset(
            ptr, constantOffset, dataLayout)) {
      key.base = base;
      key.offset = constantOffset;
      key.unknown = false;
      return key;
    }

    const llvm::Value *stripped = ptr->stripPointerCasts();
    key.base = llvm::getUnderlyingObject(stripped);
    key.offset = 0;
    key.unknown = false;

    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(stripped)) {
      llvm::APInt offAP(64, 0, true);
      if (gep->accumulateConstantOffset(dataLayout, offAP))
        key.offset = offAP.getSExtValue();
      else
        key.unknown = true;
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
        llvm::isIdentifiedObject(b.base) && a.base != b.base)
      return false;

    if (!aliasAnalysis.mayAlias(a.ptr, b.ptr))
      return false;
    return true;
  }

  RelativeOffsetInfo getRelativeOffsetInfoImpl(
      const llvm::Value *ptr, const llvm::Value *root,
      std::unordered_set<const llvm::Value *> &visited) const {
    RelativeOffsetInfo out;
    if (!ptr || !root)
      return out;
    if (!visited.insert(ptr).second)
      return out;

    const llvm::Value *stripped = ptr->stripPointerCasts();
    if (ptr == root || stripped == root) {
      out.derived = true;
      return out;
    }

    if (auto *gep = llvm::dyn_cast<llvm::GEPOperator>(ptr)) {
      out = getRelativeOffsetInfoImpl(gep->getPointerOperand(), root, visited);
      if (!out.derived || out.unknown)
        return out;
      llvm::APInt offAP(64, 0, true);
      if (gep->accumulateConstantOffset(dataLayout, offAP)) {
        out.offset += offAP.getSExtValue();
      } else {
        out.unknown = true;
      }
      return out;
    }

    if (auto *op = llvm::dyn_cast<llvm::Operator>(ptr)) {
      if (op->getOpcode() == llvm::Instruction::BitCast ||
          op->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        return getRelativeOffsetInfoImpl(op->getOperand(0), root, visited);
      }
      if (op->getOpcode() == llvm::Instruction::Select) {
        auto t = getRelativeOffsetInfoImpl(op->getOperand(1), root, visited);
        auto f = getRelativeOffsetInfoImpl(op->getOperand(2), root, visited);
        if (!t.derived && !f.derived)
          return {};
        if (t.derived && f.derived && !t.unknown && !f.unknown &&
            t.offset == f.offset)
          return t;
        out.derived = t.derived || f.derived;
        out.unknown = out.derived;
        return out;
      }
    }

    if (auto *phi = llvm::dyn_cast<llvm::PHINode>(ptr)) {
      bool saw = false;
      int64_t commonOffset = 0;
      bool commonKnown = true;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        auto incoming =
            getRelativeOffsetInfoImpl(phi->getIncomingValue(i), root, visited);
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
      return out;
    }

    return out;
  }

  std::vector<unsigned> getReachableMemBits(const llvm::Value *ptr) const {
    std::vector<unsigned> out;
    std::unordered_set<unsigned> visitedBits;
    std::unordered_set<const llvm::Value *> visitedPtrs;
    std::queue<const llvm::Value *> work;

    work.push(ptr);
    while (!work.empty()) {
      const llvm::Value *current = work.front();
      work.pop();
      if (!current || !current->getType()->isPointerTy() ||
          !visitedPtrs.insert(current).second) {
        continue;
      }

      for (unsigned memBit : getAliasMemBits(current)) {
        if (visitedBits.insert(memBit).second)
          out.push_back(memBit);
        auto it = reachablePointerSeeds.find(memBit);
        if (it == reachablePointerSeeds.end())
          continue;
        for (const auto *nextPtr : it->second)
          work.push(nextPtr);
      }

      std::vector<const llvm::Value *> ptsSet;
      if (!aliasAnalysis.getPointsToSet(current, ptsSet))
        continue;
      for (const auto *pointee : ptsSet) {
        auto baseIt = baseToPointers.find(pointee);
        if (baseIt == baseToPointers.end())
          continue;
        for (const auto *candidatePtr : baseIt->second) {
          unsigned bit = getMemBitForPtr(candidatePtr);
          if (bit != invalidBit() && visitedBits.insert(bit).second)
            out.push_back(bit);
          if (candidatePtr->getType()->isPointerTy())
            work.push(candidatePtr);
        }
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void addReachableSeed(unsigned memBit, const llvm::Value *pointerValue) {
    if (memBit == invalidBit() || !pointerValue ||
        !pointerValue->getType()->isPointerTy())
      return;
    const llvm::Value *stripped = pointerValue->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(stripped) ||
        llvm::isa<llvm::Function>(stripped) ||
        llvm::isa<llvm::UndefValue>(stripped) ||
        llvm::isa<llvm::PoisonValue>(stripped)) {
      return;
    }
    auto &seeds = reachablePointerSeeds[memBit];
    if (std::find(seeds.begin(), seeds.end(), pointerValue) == seeds.end())
      seeds.push_back(pointerValue);
  }

  void buildReachabilitySeeds() {
    std::function<void(unsigned, const llvm::Constant *)> seedFromConstant =
        [&](unsigned memBit, const llvm::Constant *constant) {
          if (!constant)
            return;
          if (constant->getType()->isPointerTy()) {
            if (!llvm::isa<llvm::ConstantPointerNull>(constant))
              addReachableSeed(memBit, constant);
            return;
          }
          for (unsigned i = 0; i < constant->getNumOperands(); ++i) {
            if (auto *nested =
                    llvm::dyn_cast<llvm::Constant>(constant->getOperand(i)))
              seedFromConstant(memBit, nested);
          }
        };

    for (auto &global : module.globals()) {
      if (global.getType()->isPointerTy()) {
        unsigned memBit = getMemBitForPtr(&global);
        if (memBit != invalidBit() && global.hasInitializer())
          seedFromConstant(memBit, global.getInitializer());
      }
    }

    for (auto &F : module) {
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto *store = llvm::dyn_cast<llvm::StoreInst>(&I);
          if (store) {
            const llvm::Value *stored = store->getValueOperand();
            if (stored && stored->getType()->isPointerTy()) {
              for (unsigned memBit :
                   getDirectAccessMemBits(store->getPointerOperand()))
                addReachableSeed(memBit, stored);
            }
            continue;
          }
          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            if (!load->getType()->isPointerTy())
              continue;
            for (unsigned memBit :
                 getDirectAccessMemBits(load->getPointerOperand()))
              addReachableSeed(memBit, load);
          }
        }
      }
    }
  }

  void buildUniverse() {
    for (auto &global : module.globals()) {
      addValueBit(&global);
      if (global.getType()->isPointerTy())
        addMemBit(&global);
    }

    for (auto &F : module) {
      for (auto &Arg : F.args()) {
        addValueBit(&Arg);
        if (Arg.getType()->isPointerTy())
          addMemBit(&Arg);
      }
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (!I.getType()->isVoidTy()) {
            addValueBit(&I);
            if (I.getType()->isPointerTy())
              addMemBit(&I);
          }
          if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            addMemBit(load->getPointerOperand());
          } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            addMemBit(store->getPointerOperand());
          } else if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I)) {
            for (auto &arg : call->args()) {
              if (arg->getType()->isPointerTy())
                addMemBit(arg.get());
            }
            if (call->getType()->isPointerTy())
              addMemBit(call);
          } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
            addMemBit(gep);
          }
        }
      }
    }
  }
};

class TaintAnalysis {
public:
  using FactType = llvm::APInt;
  using D = TaintTransformer;
  using Engine = InterEngine<TaintTransformer, TaintAnalysis>;

  static unsigned normalizeBitWidth(unsigned bit_width) {
    return bit_width == 0 ? 1u : bit_width;
  }

  TaintAnalysis(llvm::Module &M, lotus::AliasAnalysisWrapper &aa,
                const InterTaint::Options &opts)
      : module(M), info(M, aa), aliasAnalysis(aa),
        bitWidth(normalizeBitWidth(info.getBitWidth())), widthScope(bitWidth),
        entryFacts(bitWidth, 0), options(opts) {
    initializeEntryFacts();
    scanUnsupportedSpecs();
  }

  FactType getEntryValue() const { return entryFacts; }

  using Exp = Exp0<D>;
  using E = E0<D>;

  std::vector<unsigned> bitsForAccess(const llvm::Value *value,
                                      TaintSpec::AccessMode accessMode) const {
    if (accessMode == TaintSpec::VALUE) {
      unsigned bit = info.getValueBit(value);
      if (bit == TaintInfo::invalidBit())
        return {};
      return {bit};
    }
    return info.getMemoryBitsForAccess(value, accessMode);
  }

  void appendBitsForValue(std::vector<unsigned> &bits, const llvm::Value *value,
                          TaintSpec::AccessMode accessMode) const {
    auto valueBits = bitsForAccess(value, accessMode);
    bits.insert(bits.end(), valueBits.begin(), valueBits.end());
  }

  std::vector<unsigned> collectSpecBits(const llvm::CallBase &call,
                                        const TaintSpec &spec) const {
    std::vector<unsigned> bits;
    switch (spec.location) {
    case TaintSpec::RET:
      appendBitsForValue(bits, &call, spec.access_mode);
      break;
    case TaintSpec::ARG:
      if (spec.arg_index >= 0 &&
          spec.arg_index < static_cast<int>(call.arg_size()))
        appendBitsForValue(bits, call.getArgOperand(spec.arg_index),
                           spec.access_mode);
      break;
    case TaintSpec::AFTER_ARG: {
      int startIdx = spec.arg_index + 1;
      if (startIdx < 0)
        startIdx = 0;
      for (unsigned i = static_cast<unsigned>(startIdx); i < call.arg_size();
           ++i) {
        appendBitsForValue(bits, call.getArgOperand(i), spec.access_mode);
      }
      break;
    }
    }

    std::sort(bits.begin(), bits.end());
    bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    return bits;
  }

  using PointerValueList = std::vector<const llvm::Value *>;
  using PointerStoreState = std::unordered_map<unsigned, PointerValueList>;

  static void normalizePointerValues(PointerValueList &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  }

  static bool mergePointerValues(PointerValueList &dst,
                                 const PointerValueList &src) {
    const size_t oldSize = dst.size();
    dst.insert(dst.end(), src.begin(), src.end());
    normalizePointerValues(dst);
    return dst.size() != oldSize;
  }

  static bool pointerStoreStatesEqual(const PointerStoreState &lhs,
                                      const PointerStoreState &rhs) {
    if (lhs.size() != rhs.size())
      return false;
    for (const auto &entry : lhs) {
      auto it = rhs.find(entry.first);
      if (it == rhs.end() || entry.second != it->second)
        return false;
    }
    return true;
  }

  static bool mergePointerStoreState(PointerStoreState &dst,
                                     const PointerStoreState &src) {
    bool changed = false;
    for (const auto &entry : src) {
      auto &dstValues = dst[entry.first];
      changed = mergePointerValues(dstValues, entry.second) || changed;
    }
    return changed;
  }

  PointerValueList collectStoredPointerValues(const llvm::Value *stored) const {
    PointerValueList out;
    if (!stored || !stored->getType()->isPointerTy())
      return out;

    const llvm::Value *stripped = stored->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(stripped) ||
        llvm::isa<llvm::Function>(stripped) ||
        llvm::isa<llvm::UndefValue>(stripped) ||
        llvm::isa<llvm::PoisonValue>(stripped)) {
      return out;
    }

    out.push_back(stored);
    normalizePointerValues(out);
    return out;
  }

  PointerStoreState buildInitialPointerStoreState() const {
    PointerStoreState out;
    for (auto &global : module.globals()) {
      if (!global.getValueType()->isPointerTy() || !global.hasInitializer())
        continue;
      unsigned memBit = TaintInfo::invalidBit();
      if (!info.getPreciseDirectMemBit(&global, memBit))
        continue;
      out[memBit] = collectStoredPointerValues(global.getInitializer());
    }
    return out;
  }

  PointerValueList
  collectStoredPointerValuesAtCall(const llvm::Value *stored,
                                   const llvm::CallBase &call,
                                   const llvm::Function &callee) const {
    PointerValueList out;
    if (!stored || !stored->getType()->isPointerTy())
      return out;

    const llvm::Value *stripped = stored->stripPointerCasts();
    if (llvm::isa<llvm::ConstantPointerNull>(stripped) ||
        llvm::isa<llvm::Function>(stripped) ||
        llvm::isa<llvm::UndefValue>(stripped) ||
        llvm::isa<llvm::PoisonValue>(stripped)) {
      return out;
    }

    unsigned numArgs = static_cast<unsigned>(
        std::min<size_t>(call.arg_size(), callee.arg_size()));
    const auto *formalIt = callee.arg_begin();
    for (unsigned i = 0; i < numArgs; ++i, ++formalIt) {
      const llvm::Value *formal = &*formalIt;
      auto relative = info.getRelativeOffsetInfo(stored, formal);
      if (!relative.derived)
        continue;
      const llvm::Value *actual = call.getArgOperand(i);
      if (!actual || !actual->getType()->isPointerTy())
        continue;
      out.push_back(actual);
    }

    if (out.empty())
      return collectStoredPointerValues(stored);

    normalizePointerValues(out);
    return out;
  }

  std::vector<unsigned>
  collectCallStoreTargetBits(const llvm::StoreInst &store,
                             const llvm::CallBase &call,
                             const llvm::Function &callee) const {
    std::vector<unsigned> out;
    bool mappedToCaller = false;

    unsigned numArgs = static_cast<unsigned>(
        std::min<size_t>(call.arg_size(), callee.arg_size()));
    const auto *formalIt = callee.arg_begin();
    for (unsigned i = 0; i < numArgs; ++i, ++formalIt) {
      if (!formalIt->getType()->isPointerTy())
        continue;
      auto relative =
          info.getRelativeOffsetInfo(store.getPointerOperand(), &*formalIt);
      if (!relative.derived)
        continue;
      mappedToCaller = true;
      const llvm::Value *actual = call.getArgOperand(i);
      auto mappedBits = info.getCallerBitsForRelativeAccess(actual, relative,
                                                            call.getFunction());
      out.insert(out.end(), mappedBits.begin(), mappedBits.end());
    }

    if (!mappedToCaller) {
      const llvm::Value *base = llvm::getUnderlyingObject(
          store.getPointerOperand()->stripPointerCasts());
      if (llvm::isa<llvm::GlobalValue>(base)) {
        auto directBits =
            info.getDirectAccessMemBits(store.getPointerOperand());
        out.insert(out.end(), directBits.begin(), directBits.end());
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void applyCalleePointerStoreEffects(PointerStoreState &state,
                                      const llvm::CallBase &call,
                                      const llvm::Function &callee,
                                      bool allowStrongUpdate) const {
    for (const auto &BB : callee) {
      for (const auto &I : BB) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(&I);
        if (!store || !store->getValueOperand()->getType()->isPointerTy())
          continue;

        auto targetBits = collectCallStoreTargetBits(*store, call, callee);
        if (targetBits.empty())
          continue;

        PointerValueList storedValues = collectStoredPointerValuesAtCall(
            store->getValueOperand(), call, callee);

        if (allowStrongUpdate && targetBits.size() == 1) {
          state[targetBits.front()] = std::move(storedValues);
          continue;
        }

        if (storedValues.empty())
          continue;

        for (unsigned memBit : targetBits)
          mergePointerValues(state[memBit], storedValues);
      }
    }
  }

  void applyPointerStoreTransfer(PointerStoreState &state,
                                 const llvm::Instruction &I) const {
    if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I)) {
      auto possibleCallees = getPossibleCallees(module, *call);
      size_t definedCalleeCount = 0;
      for (const auto *callee : possibleCallees) {
        if (callee && !callee->isDeclaration())
          ++definedCalleeCount;
      }
      const bool allowStrongUpdate = definedCalleeCount == 1;
      for (const auto *callee : possibleCallees) {
        if (!callee || callee->isDeclaration())
          continue;
        applyCalleePointerStoreEffects(state, *call, *callee,
                                       allowStrongUpdate);
      }
      return;
    }

    auto *store = llvm::dyn_cast<llvm::StoreInst>(&I);
    if (!store || !store->getValueOperand()->getType()->isPointerTy())
      return;

    PointerValueList storedValues =
        collectStoredPointerValues(store->getValueOperand());

    unsigned preciseBit = TaintInfo::invalidBit();
    if (info.getPreciseDirectMemBit(store->getPointerOperand(), preciseBit)) {
      state[preciseBit] = std::move(storedValues);
      return;
    }

    if (storedValues.empty())
      return;

    for (unsigned memBit :
         info.getDirectAccessMemBits(store->getPointerOperand()))
      mergePointerValues(state[memBit], storedValues);
  }

  std::vector<unsigned>
  getReachableMemBitsAt(const llvm::Value *ptr,
                        const PointerStoreState &state) const {
    std::vector<unsigned> out;
    std::unordered_set<unsigned> visitedBits;
    std::unordered_set<const llvm::Value *> visitedPtrs;
    std::queue<const llvm::Value *> work;

    work.push(ptr);
    while (!work.empty()) {
      const llvm::Value *current = work.front();
      work.pop();
      if (!current || !current->getType()->isPointerTy() ||
          !visitedPtrs.insert(current).second) {
        continue;
      }

      for (unsigned memBit : info.getAliasMemBits(current)) {
        if (visitedBits.insert(memBit).second)
          out.push_back(memBit);

        auto dynamicIt = state.find(memBit);
        const bool hasDynamicValues =
            dynamicIt != state.end() && !dynamicIt->second.empty();
        if (const auto *staticSeeds = info.findStaticReachableSeeds(memBit)) {
          if (dynamicIt == state.end() || hasDynamicValues) {
            for (const auto *nextPtr : *staticSeeds)
              work.push(nextPtr);
          }
        }
        if (hasDynamicValues) {
          for (const auto *nextPtr : dynamicIt->second)
            work.push(nextPtr);
        }
      }

      std::vector<const llvm::Value *> ptsSet;
      if (!aliasAnalysis.getPointsToSet(current, ptsSet))
        continue;
      for (const auto *pointee : ptsSet) {
        const auto *candidates = info.findPointersForBase(pointee);
        if (!candidates)
          continue;
        for (const auto *candidatePtr : *candidates) {
          unsigned bit = info.getMemBitForPtr(candidatePtr);
          if (bit != TaintInfo::invalidBit() && visitedBits.insert(bit).second)
            out.push_back(bit);
          if (candidatePtr->getType()->isPointerTy())
            work.push(candidatePtr);
        }
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void killBit(D::value_type &transfer, unsigned bit) const {
    if (bit >= bitWidth)
      return;
    transfer.rel[bit] = llvm::APInt(bitWidth, 0);
    for (auto &row : transfer.rel)
      row.clearBit(bit);
    transfer.gen.clearBit(bit);
  }

  void killAccess(D::value_type &transfer, const llvm::Value *value,
                  TaintSpec::AccessMode mode) const {
    for (unsigned bit : bitsForAccess(value, mode))
      killBit(transfer, bit);
  }

  bool addValueFlow(D::value_type &transfer, const llvm::Value *src,
                    const llvm::Value *dst) const {
    unsigned srcBit = info.getValueBit(src);
    unsigned dstBit = info.getValueBit(dst);
    if (srcBit == TaintInfo::invalidBit() || dstBit == TaintInfo::invalidBit())
      return false;
    D::addEdge(transfer, srcBit, dstBit);
    return true;
  }

  bool addMemoryIdentityFlow(D::value_type &transfer, const llvm::Value *src,
                             const llvm::Value *dst) const {
    if (!src || !dst || !src->getType()->isPointerTy() ||
        !dst->getType()->isPointerTy())
      return false;
    bool updated = false;
    auto dstBits = info.getDirectAccessMemBits(dst);
    for (unsigned srcBit : info.getDirectAccessMemBits(src)) {
      for (unsigned dstBit : dstBits) {
        D::addEdge(transfer, srcBit, dstBit);
        updated = true;
      }
    }
    return updated;
  }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    auto *firstPhi = llvm::dyn_cast<llvm::PHINode>(BB.begin());
    if (!firstPhi)
      return inExpr;

    E result = nullptr;
    for (auto *pred : predecessors(&BB)) {
      D::value_type transfer = D::one();
      bool updated = false;
      for (auto &Inst : BB) {
        auto *phi = llvm::dyn_cast<llvm::PHINode>(&Inst);
        if (!phi)
          break;
        const llvm::Value *incoming = phi->getIncomingValueForBlock(pred);
        updated = addValueFlow(transfer, incoming, phi) || updated;
        updated = addMemoryIdentityFlow(transfer, incoming, phi) || updated;
      }

      E branch = Exp::hole(Engine::getBlockSymbol(pred));
      if (updated)
        branch = Exp::seq(transfer, branch);
      result = result ? Exp::ndet(result, branch) : branch;
    }
    return result ? result : inExpr;
  }

  E getTransfer(llvm::Instruction &I, E currentPath) {
    if (llvm::isa<llvm::CallBase>(&I) || llvm::isa<llvm::PHINode>(&I))
      return currentPath;

    D::value_type transfer = D::one();
    bool updated = false;

    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&I)) {
      const llvm::Value *value = store->getValueOperand();
      const llvm::Value *ptr = store->getPointerOperand();

      unsigned preciseMemBit = TaintInfo::invalidBit();
      if (info.getPreciseDirectMemBit(ptr, preciseMemBit)) {
        killBit(transfer, preciseMemBit);
        updated = true;
      }

      unsigned valueBit = info.getValueBit(value);
      if (valueBit != TaintInfo::invalidBit()) {
        for (unsigned memBit : info.getDirectAccessMemBits(ptr)) {
          D::addEdge(transfer, valueBit, memBit);
          updated = true;
        }
      }
      if (auto *global =
              llvm::dyn_cast<llvm::GlobalVariable>(ptr->stripPointerCasts())) {
        unsigned globalBit = info.getValueBit(global);
        if (globalBit != TaintInfo::invalidBit()) {
          killBit(transfer, globalBit);
          updated = true;
        }
        updated = addValueFlow(transfer, value, global) || updated;
      }
    } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
      const llvm::Value *ptr = load->getPointerOperand();
      unsigned loadBit = info.getValueBit(load);
      (void)loadBit;

      if (options.propagate_pointer_value_on_load) {
        unsigned ptrBit = info.getValueBit(ptr);
        if (ptrBit != TaintInfo::invalidBit() &&
            loadBit != TaintInfo::invalidBit()) {
          D::addEdge(transfer, ptrBit, loadBit);
          updated = true;
        }
      }

      for (unsigned memBit : info.getDirectAccessMemBits(ptr)) {
        D::addEdge(transfer, memBit, loadBit);
        updated = true;
      }
      if (auto *global =
              llvm::dyn_cast<llvm::GlobalVariable>(ptr->stripPointerCasts())) {
        updated = addValueFlow(transfer, global, load) || updated;
      }
    } else if (auto *binop = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
      unsigned lhsBit = info.getValueBit(binop->getOperand(0));
      unsigned rhsBit = info.getValueBit(binop->getOperand(1));
      unsigned outBit = info.getValueBit(binop);
      if (outBit != TaintInfo::invalidBit()) {
        if (lhsBit != TaintInfo::invalidBit()) {
          D::addEdge(transfer, lhsBit, outBit);
          updated = true;
        }
        if (rhsBit != TaintInfo::invalidBit()) {
          D::addEdge(transfer, rhsBit, outBit);
          updated = true;
        }
      }
    } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&I)) {
      unsigned inBit = info.getValueBit(cast->getOperand(0));
      unsigned outBit = info.getValueBit(cast);
      if (inBit != TaintInfo::invalidBit() &&
          outBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, inBit, outBit);
        updated = true;
      }
      updated =
          addMemoryIdentityFlow(transfer, cast->getOperand(0), cast) || updated;
    } else if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&I)) {
      unsigned inBit = info.getValueBit(gep->getPointerOperand());
      unsigned outBit = info.getValueBit(gep);
      if (inBit != TaintInfo::invalidBit() &&
          outBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, inBit, outBit);
        updated = true;
      }
      updated =
          addMemoryIdentityFlow(transfer, gep->getPointerOperand(), gep) ||
          updated;
    } else if (auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(&I)) {
      updated = addValueFlow(transfer, cmp->getOperand(0), cmp) || updated;
      updated = addValueFlow(transfer, cmp->getOperand(1), cmp) || updated;
    } else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(&I)) {
      updated =
          addValueFlow(transfer, select->getCondition(), select) || updated;
      updated =
          addValueFlow(transfer, select->getTrueValue(), select) || updated;
      updated =
          addValueFlow(transfer, select->getFalseValue(), select) || updated;
      updated =
          addMemoryIdentityFlow(transfer, select->getTrueValue(), select) ||
          updated;
      updated =
          addMemoryIdentityFlow(transfer, select->getFalseValue(), select) ||
          updated;
    } else if (auto *unary = llvm::dyn_cast<llvm::UnaryOperator>(&I)) {
      updated = addValueFlow(transfer, unary->getOperand(0), unary) || updated;
      updated = addMemoryIdentityFlow(transfer, unary->getOperand(0), unary) ||
                updated;
    } else if (auto *insert = llvm::dyn_cast<llvm::InsertValueInst>(&I)) {
      updated = addValueFlow(transfer, insert->getAggregateOperand(), insert) ||
                updated;
      updated =
          addValueFlow(transfer, insert->getInsertedValueOperand(), insert) ||
          updated;
    } else if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(&I)) {
      updated =
          addValueFlow(transfer, extract->getAggregateOperand(), extract) ||
          updated;
    } else if (auto *insertElem = llvm::dyn_cast<llvm::InsertElementInst>(&I)) {
      updated = addValueFlow(transfer, insertElem->getOperand(0), insertElem) ||
                updated;
      updated = addValueFlow(transfer, insertElem->getOperand(1), insertElem) ||
                updated;
    } else if (auto *extractElem =
                   llvm::dyn_cast<llvm::ExtractElementInst>(&I)) {
      updated = addValueFlow(transfer, extractElem->getVectorOperand(),
                             extractElem) ||
                updated;
    } else if (auto *shuffle = llvm::dyn_cast<llvm::ShuffleVectorInst>(&I)) {
      updated =
          addValueFlow(transfer, shuffle->getOperand(0), shuffle) || updated;
      updated =
          addValueFlow(transfer, shuffle->getOperand(1), shuffle) || updated;
    }

    if (!updated)
      return currentPath;
    return Exp::seq(transfer, currentPath);
  }

  D::value_type getCallEntryTransfer(const llvm::CallBase &call,
                                     const llvm::Function &callee) {
    D::value_type transfer = D::one();
    unsigned numArgs = static_cast<unsigned>(
        std::min<size_t>(call.arg_size(), callee.arg_size()));
    const auto *paramIt = callee.arg_begin();
    for (unsigned i = 0; i < numArgs; ++i, ++paramIt) {
      const llvm::Value *arg = call.getArgOperand(i);
      unsigned argBit = info.getValueBit(arg);
      unsigned paramBit = info.getValueBit(&*paramIt);
      if (argBit != TaintInfo::invalidBit() &&
          paramBit != TaintInfo::invalidBit()) {
        D::addEdge(transfer, argBit, paramBit);
      }

      if (arg && arg->getType()->isPointerTy()) {
        unsigned paramMemBit = info.getMemBitForPtr(&*paramIt);
        if (paramMemBit != TaintInfo::invalidBit()) {
          for (unsigned memBit : info.getDirectAccessMemBits(arg)) {
            D::addEdge(transfer, memBit, paramMemBit);
          }
        }
      }
    }
    return transfer;
  }

  D::value_type getCallReturnTransfer(const llvm::CallBase &call,
                                      const llvm::Function &callee) {
    D::value_type transfer = D::one();
    std::vector<unsigned> callReachableBits;
    if (call.getType()->isPointerTy())
      callReachableBits = bitsForAccess(&call, TaintSpec::REACHABLE_DEREF);

    if (!call.getType()->isVoidTy()) {
      unsigned callBit = info.getValueBit(&call);
      if (callBit != TaintInfo::invalidBit()) {
        for (const auto &BB : callee) {
          if (auto *ret =
                  llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
            if (const llvm::Value *retVal = ret->getReturnValue()) {
              unsigned retBit = info.getValueBit(retVal);
              if (retBit != TaintInfo::invalidBit()) {
                D::addEdge(transfer, retBit, callBit);
              }

              if (retVal->getType()->isPointerTy() &&
                  !callReachableBits.empty()) {
                for (unsigned sourceBit :
                     bitsForAccess(retVal, TaintSpec::REACHABLE_DEREF)) {
                  for (unsigned targetBit : callReachableBits)
                    D::addEdge(transfer, sourceBit, targetBit);
                }
              }
            }
          }
        }
      }
    }

    unsigned numArgs = static_cast<unsigned>(
        std::min<size_t>(call.arg_size(), callee.arg_size()));
    const auto *paramIt = callee.arg_begin();
    for (unsigned i = 0; i < numArgs; ++i, ++paramIt) {
      const llvm::Value *arg = call.getArgOperand(i);
      if (!arg || !arg->getType()->isPointerTy())
        continue;
      unsigned paramMemBit = info.getMemBitForPtr(&*paramIt);
      if (paramMemBit == TaintInfo::invalidBit())
        continue;
      for (unsigned memBit : info.getDirectAccessMemBits(arg))
        D::addEdge(transfer, paramMemBit, memBit);
    }

    for (const auto *candidate : info.getMemoryPointers()) {
      if (info.getMemBitForPtr(candidate) == TaintInfo::invalidBit())
        continue;
      if (TaintInfo::getOwningFunction(candidate) != &callee)
        continue;

      const auto *formalIt = callee.arg_begin();
      for (unsigned i = 0; i < numArgs; ++i, ++formalIt) {
        if (!formalIt->getType()->isPointerTy())
          continue;
        auto relative = info.getRelativeOffsetInfo(candidate, &*formalIt);
        if (!relative.derived)
          continue;

        const llvm::Value *actual = call.getArgOperand(i);
        const llvm::Function *caller = call.getFunction();
        auto targetBits =
            info.getCallerBitsForRelativeAccess(actual, relative, caller);
        for (unsigned sourceBit : info.getDirectAccessMemBits(candidate)) {
          for (unsigned targetBit : targetBits)
            D::addEdge(transfer, sourceBit, targetBit);
        }
      }
    }

    applySourceSpecs(call, callee, transfer);
    applyPipeSpecs(call, callee, transfer);
    return transfer;
  }

  D::value_type getCallToReturnTransfer(const llvm::CallBase &call) {
    return buildCallToReturnSpecTransfer(call);
  }

  FactType applySummary(const D::value_type &summary, const FactType &fact) {
    return D::apply(summary, fact);
  }

  FactType joinFacts(const FactType &a, const FactType &b) { return a | b; }

  bool factsEqual(const FactType &a, const FactType &b) { return a == b; }

  bool hasUnsupportedSpecs() const { return unsupportedSpecsEncountered; }

  const std::unordered_map<const llvm::Value *, unsigned> &
  getValueBits() const {
    return info.getValueBits();
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  buildPointerMemoryBits() const {
    return info.buildPointerMemoryBits();
  }

  std::unordered_map<const llvm::Value *, std::vector<unsigned>>
  buildReachablePointerMemoryBits() const {
    return info.buildReachablePointerMemoryBits();
  }

  std::map<BlockKey,
           std::unordered_map<const llvm::Value *, std::vector<unsigned>>>
  buildFlowSensitiveReachablePointerMemoryBits() const {
    std::map<BlockKey,
             std::unordered_map<const llvm::Value *, std::vector<unsigned>>>
        out;

    PointerStoreState initialState = buildInitialPointerStoreState();
    for (auto &F : module) {
      if (F.isDeclaration())
        continue;

      std::unordered_map<const llvm::BasicBlock *, PointerStoreState>
          exitStates;
      std::queue<const llvm::BasicBlock *> worklist;
      std::unordered_set<const llvm::BasicBlock *> inWorklist;

      worklist.push(&F.getEntryBlock());
      inWorklist.insert(&F.getEntryBlock());

      while (!worklist.empty()) {
        const llvm::BasicBlock *BB = worklist.front();
        worklist.pop();
        inWorklist.erase(BB);

        PointerStoreState entryState;
        if (BB == &F.getEntryBlock())
          entryState = initialState;
        for (const auto *pred : predecessors(BB)) {
          auto predIt = exitStates.find(pred);
          if (predIt != exitStates.end())
            mergePointerStoreState(entryState, predIt->second);
        }

        PointerStoreState currentState = entryState;
        for (const auto &I : *BB)
          applyPointerStoreTransfer(currentState, I);

        auto exitIt = exitStates.find(BB);
        if (exitIt != exitStates.end() &&
            pointerStoreStatesEqual(exitIt->second, currentState)) {
          continue;
        }
        exitStates[BB] = currentState;

        for (const auto *succ : llvm::successors(BB)) {
          if (inWorklist.insert(succ).second)
            worklist.push(succ);
        }
      }

      for (auto &BB : F) {
        auto exitIt = exitStates.find(&BB);
        if (exitIt == exitStates.end())
          continue;

        auto &blockBits = out[BlockKey{&BB}];
        for (const auto *ptr : info.getMemoryPointers())
          blockBits.emplace(ptr, getReachableMemBitsAt(ptr, exitIt->second));
      }
    }

    return out;
  }

private:
  llvm::Module &module;
  TaintInfo info;
  lotus::AliasAnalysisWrapper &aliasAnalysis;
  unsigned bitWidth = 1;
  D::WidthScope widthScope;
  FactType entryFacts;
  InterTaint::Options options;
  bool unsupportedSpecsEncountered = false;

  void markUnsupportedSpec(const llvm::Function &callee, const char *kind) {
    unsupportedSpecsEncountered = true;
    llvm::errs() << "[npa-taint] unsupported non-taint " << kind << " spec on '"
                 << callee.getName() << "'\n";
  }

  bool containsUnsupportedSpec(const llvm::Function &callee) {
    std::string funcName = taint_config::normalize_name(callee.getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    if (!cfg)
      return false;

    for (const auto &spec : cfg->source_specs) {
      if (spec.taint_type != TaintSpec::TAINTED) {
        markUnsupportedSpec(callee, "source");
        return true;
      }
    }
    for (const auto &spec : cfg->sink_specs) {
      if (spec.taint_type != TaintSpec::TAINTED) {
        markUnsupportedSpec(callee, "sink");
        return true;
      }
    }
    return false;
  }

  void scanUnsupportedSpecs() {
    std::unordered_set<const llvm::Function *> scanned;
    for (auto &F : module) {
      if (F.isDeclaration())
        continue;
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto *call = llvm::dyn_cast<llvm::CallBase>(&I);
          if (!call)
            continue;
          for (const auto *callee : getSpecCandidateCallees(*call)) {
            if (callee && scanned.insert(callee).second &&
                containsUnsupportedSpec(*callee)) {
              break;
            }
          }
        }
      }
    }
  }

  void initializeEntryFacts() {
    if (!options.seed_main_pointer_args)
      return;
    if (auto *Main = module.getFunction("main")) {
      for (auto &Arg : Main->args()) {
        if (Arg.getType()->isPointerTy()) {
          unsigned bit = info.getValueBit(&Arg);
          if (bit != TaintInfo::invalidBit())
            entryFacts.setBit(bit);
        }
      }
    }
  }

  std::vector<const llvm::Function *>
  getSpecCandidateCallees(const llvm::CallBase &call) const {
    std::unordered_set<const llvm::Value *> visitedConstantBacked;
    std::function<void(const llvm::Value *,
                       std::vector<const llvm::Function *> &)>
        addConstantBackedTargets =
            [&](const llvm::Value *value,
                std::vector<const llvm::Function *> &targets) {
              if (!value)
                return;
              const llvm::Value *stripped = value->stripPointerCasts();
              if (!visitedConstantBacked.insert(stripped).second)
                return;
              if (auto *function = llvm::dyn_cast<llvm::Function>(stripped)) {
                targets.push_back(function);
                return;
              }
              if (auto *global =
                      llvm::dyn_cast<llvm::GlobalVariable>(stripped)) {
                if (global->hasInitializer())
                  addConstantBackedTargets(global->getInitializer(), targets);
                return;
              }
              if (auto *load = llvm::dyn_cast<llvm::LoadInst>(stripped)) {
                addConstantBackedTargets(load->getPointerOperand(), targets);
                return;
              }
              if (auto *constant = llvm::dyn_cast<llvm::Constant>(stripped)) {
                for (unsigned i = 0; i < constant->getNumOperands(); ++i)
                  addConstantBackedTargets(constant->getOperand(i), targets);
              }
            };

    std::vector<const llvm::Function *> out;
    std::vector<const llvm::Function *> strongTargets;
    if (const auto *direct = call.getCalledFunction()) {
      out.push_back(direct);
      return out;
    }

    std::vector<const llvm::Function *> aaTargets;
    aliasAnalysis.getIndirectCallTargets(const_cast<llvm::CallBase *>(&call),
                                         aaTargets);
    for (const auto *callee : aaTargets) {
      if (callee)
        strongTargets.push_back(callee);
    }

    if (auto *calledOperand = call.getCalledOperand()) {
      auto *stripped = calledOperand->stripPointerCasts();
      if (auto *direct = llvm::dyn_cast<llvm::Function>(stripped))
        strongTargets.push_back(direct);
      addConstantBackedTargets(calledOperand, strongTargets);
    }

    if (!strongTargets.empty())
      out = std::move(strongTargets);
    else {
      auto possible = Engine::getPossibleCallees(module, call);
      for (const auto *callee : possible) {
        if (callee)
          out.push_back(callee);
      }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }

  void applySourceSpecs(const llvm::CallBase &call,
                        const llvm::Function &callee,
                        D::value_type &transfer) const {
    std::string funcName = taint_config::normalize_name(callee.getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    if (!cfg || !cfg->has_source_specs())
      return;

    for (const auto &spec : cfg->source_specs) {
      if (spec.taint_type != TaintSpec::TAINTED)
        continue;
      for (unsigned bit : collectSpecBits(call, spec))
        D::addGen(transfer, bit);
    }
  }

  void applyPipeSpecs(const llvm::CallBase &call, const llvm::Function &callee,
                      D::value_type &transfer) const {
    std::string funcName = taint_config::normalize_name(callee.getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    if (!cfg || !cfg->has_pipe_specs())
      return;

    for (const auto &pipe : cfg->pipe_specs) {
      auto sourceBits = collectSpecBits(call, pipe.from);
      auto targetBits = collectSpecBits(call, pipe.to);
      if (targetBits.empty() || sourceBits.empty())
        continue;

      for (unsigned sourceBit : sourceBits) {
        for (unsigned targetBit : targetBits)
          D::addEdge(transfer, sourceBit, targetBit);
      }
    }
  }

  D::value_type buildSpecTransferForCallee(const llvm::CallBase &call,
                                           const llvm::Function &callee,
                                           bool include_extra_specs) const {
    (void)include_extra_specs;
    D::value_type transfer = D::one();
    applySourceSpecs(call, callee, transfer);
    applyPipeSpecs(call, callee, transfer);
    return transfer;
  }

  D::value_type
  buildCallToReturnSpecTransfer(const llvm::CallBase &call) const {
    auto callees = getSpecCandidateCallees(call);
    if (callees.empty())
      return D::one();

    D::value_type combined = D::zero();
    for (const auto *callee : callees) {
      if (!callee)
        continue;
      combined =
          D::combine(combined, buildSpecTransferForCallee(call, *callee, true));
    }
    return combined;
  }

public:
  D::value_type buildNormalTransfer(const llvm::Instruction &instruction) {
    if (llvm::isa<llvm::CallBase>(&instruction))
      return D::one();
    auto current = Exp::term(D::one());
    auto next =
        getTransfer(const_cast<llvm::Instruction &>(instruction), current);
    if (!next || next == current || next->k != Exp0<D>::Seq)
      return D::one();
    return next->c;
  }

  D::value_type buildCallTransfer(
      const llvm::CallBase &call,
      const std::unordered_map<const llvm::Function *, D::value_type>
          &summaries) {
    auto callees = getPossibleCallees(module, call);
    auto explicitCandidates = getSpecCandidateCallees(call);
    const bool hasUnresolvedIndirectTarget =
        callees.empty() && call.getCalledFunction() == nullptr;

    D::value_type combined = D::zero();
    for (const auto *callee : callees) {
      auto it = summaries.find(callee);
      if (it == summaries.end())
        continue;
      D::value_type effect =
          D::extend(getCallReturnTransfer(call, *callee),
                    D::extend(it->second, getCallEntryTransfer(call, *callee)));
      combined = D::combine(combined, effect);
    }

    bool sawDeclarationWithoutSummary = false;
    for (const auto *candidate : explicitCandidates) {
      if (!candidate || !candidate->isDeclaration())
        continue;
      if (summaries.find(candidate) == summaries.end() &&
          !hasExternalTransferSpec(*candidate)) {
        sawDeclarationWithoutSummary = true;
        break;
      }
    }
    if (sawDeclarationWithoutSummary)
      combined = D::combine(combined, getConservativeExternalTransfer(call));
    if (hasUnresolvedIndirectTarget)
      combined = D::combine(combined, getConservativeExternalTransfer(call));
    combined = D::combine(combined, getCallFallbackTransfer(call, callees));
    if (D::equal(combined, D::zero()))
      return getCallToReturnTransfer(call);
    return combined;
  }

  std::vector<llvm::Function *>
  getPossibleCallees(llvm::Module &M, const llvm::CallBase &call) const {
    std::vector<llvm::Function *> explicitTargetsResolved;
    auto explicitTargets = getSpecCandidateCallees(call);
    for (const auto *target : explicitTargets) {
      if (!target)
        continue;
      explicitTargetsResolved.push_back(const_cast<llvm::Function *>(target));
    }
    std::sort(explicitTargetsResolved.begin(), explicitTargetsResolved.end());
    explicitTargetsResolved.erase(std::unique(explicitTargetsResolved.begin(),
                                              explicitTargetsResolved.end()),
                                  explicitTargetsResolved.end());
    if (!explicitTargetsResolved.empty())
      return explicitTargetsResolved;
    return Engine::getPossibleCallees(M, call);
  }

  D::value_type
  getConservativeExternalTransfer(const llvm::CallBase &call) const {
    (void)call;
    return D::one();
  }

  bool hasExternalTransferSpec(const llvm::Function &callee) const {
    std::string funcName = taint_config::normalize_name(callee.getName().str());
    const FunctionTaintConfig *cfg =
        taint_config::get_function_config(funcName);
    return cfg && (cfg->has_source_specs() || cfg->has_pipe_specs());
  }

  D::value_type
  getCallFallbackTransfer(const llvm::CallBase &call,
                          const std::vector<llvm::Function *> &resolved) const {
    std::unordered_set<const llvm::Function *> resolvedSet(resolved.begin(),
                                                           resolved.end());
    D::value_type combined = D::zero();
    bool sawFallback = false;
    if (resolved.empty() && call.getCalledFunction() == nullptr) {
      combined = D::combine(combined, getConservativeExternalTransfer(call));
      sawFallback = true;
    }
    for (const auto *callee : resolved) {
      if (!callee || !callee->isDeclaration())
        continue;
      if (hasExternalTransferSpec(*callee))
        continue;
      combined = D::combine(combined, getConservativeExternalTransfer(call));
      sawFallback = true;
      break;
    }
    for (const auto *candidate : getSpecCandidateCallees(call)) {
      if (!candidate)
        continue;
      if (resolvedSet.count(candidate))
        continue;
      combined = D::combine(combined,
                            buildSpecTransferForCallee(call, *candidate, true));
      sawFallback = true;
    }
    return sawFallback ? combined : D::zero();
  }

  std::vector<std::string> triggeredSinkInputs(const llvm::CallBase &call,
                                               const FactType &preFact,
                                               const FactType &postFact) const {
    std::set<std::string> hits;
    auto callees = getSpecCandidateCallees(call);
    for (const auto *callee : callees) {
      if (!callee)
        continue;
      auto *cfg = taint_config::get_function_config(
          taint_config::normalize_name(callee->getName().str()));
      if (!cfg || !cfg->has_sink_specs())
        continue;

      auto checkSpec = [&](const TaintSpec &spec, const FactType &fact,
                           bool allowRet) {
        if (spec.location == TaintSpec::RET) {
          if (!allowRet || call.getType()->isVoidTy())
            return;
          for (unsigned bit : bitsForAccess(&call, spec.access_mode)) {
            if (bit < fact.getBitWidth() && fact[bit])
              hits.insert(spec.access_mode == TaintSpec::VALUE ? "ret"
                                                               : "ret(mem)");
          }
          return;
        }

        auto checkArg = [&](unsigned idx) {
          if (idx >= call.arg_size())
            return;
          const llvm::Value *arg = call.getArgOperand(idx);
          for (unsigned bit : bitsForAccess(arg, spec.access_mode)) {
            if (bit < fact.getBitWidth() && fact[bit]) {
              std::string label = "arg" + std::to_string(idx);
              if (spec.access_mode == TaintSpec::DIRECT_DEREF)
                label += "(mem)";
              else if (spec.access_mode == TaintSpec::REACHABLE_DEREF)
                label += "(reachable)";
              hits.insert(std::move(label));
              break;
            }
          }
        };

        if (spec.location == TaintSpec::ARG) {
          if (spec.arg_index < 0)
            return;
          checkArg(static_cast<unsigned>(spec.arg_index));
          return;
        }

        int startIdx = spec.arg_index + 1;
        if (startIdx < 0)
          startIdx = 0;
        for (unsigned i = static_cast<unsigned>(startIdx); i < call.arg_size();
             ++i)
          checkArg(i);
      };

      for (const auto &spec : cfg->sink_specs) {
        if (spec.taint_type != TaintSpec::TAINTED)
          continue;
        if (spec.location == TaintSpec::RET)
          checkSpec(spec, postFact, true);
        else
          checkSpec(spec, preFact, false);
      }
    }

    return {hits.begin(), hits.end()};
  }
};

static const llvm::APInt *findFactForBlock(const InterTaint::Result &result,
                                           const llvm::BasicBlock *block) {
  auto exitIt = result.blockExitFacts.find(BlockKey{block});
  if (exitIt != result.blockExitFacts.end())
    return &exitIt->second;
  auto entryIt = result.blockFacts.find(BlockKey{block});
  if (entryIt != result.blockFacts.end())
    return &entryIt->second;
  return nullptr;
}

bool InterTaint::Result::isValueTainted(const llvm::BasicBlock *block,
                                        const llvm::Value *value) const {
  auto bitIt = valueBits.find(value);
  if (bitIt == valueBits.end())
    return false;

  const llvm::APInt *fact = findFactForBlock(*this, block);
  return fact && bitIt->second < fact->getBitWidth() && (*fact)[bitIt->second];
}

bool InterTaint::Result::isMemoryTainted(const llvm::BasicBlock *block,
                                         const llvm::Value *pointer) const {
  auto bitsIt = pointerMemoryBits.find(pointer);
  if (bitsIt == pointerMemoryBits.end())
    return false;

  const llvm::APInt *fact = findFactForBlock(*this, block);
  if (!fact)
    return false;
  for (unsigned bit : bitsIt->second) {
    if (bit < fact->getBitWidth() && (*fact)[bit])
      return true;
  }
  return false;
}

bool InterTaint::Result::isReachableMemoryTainted(
    const llvm::BasicBlock *block, const llvm::Value *pointer) const {
  const llvm::APInt *fact = findFactForBlock(*this, block);
  if (!fact)
    return false;

  const std::vector<unsigned> *bits = nullptr;
  auto blockIt = blockReachablePointerMemoryBits.find(BlockKey{block});
  if (blockIt != blockReachablePointerMemoryBits.end()) {
    auto ptrIt = blockIt->second.find(pointer);
    if (ptrIt != blockIt->second.end())
      bits = &ptrIt->second;
  }
  if (!bits) {
    auto bitsIt = reachablePointerMemoryBits.find(pointer);
    if (bitsIt == reachablePointerMemoryBits.end())
      return false;
    bits = &bitsIt->second;
  }

  for (unsigned bit : *bits) {
    if (bit < fact->getBitWidth() && (*fact)[bit])
      return true;
  }
  return false;
}

bool InterTaint::Result::isSinkTriggered(const llvm::CallBase *call) const {
  for (const auto &hit : sinkHits) {
    if (hit.call == call)
      return true;
  }
  return false;
}

void InterTaint::Result::reportVulnerabilities(llvm::raw_ostream &os) const {
  os << "\nNPA Taint Analysis Results:\n";
  os << "===========================\n";
  if (sinkHits.empty()) {
    os << "No reachable sinks detected.\n";
    return;
  }
  os << "Summary: " << sinkHits.size() << " reachable sinks detected.\n";
  for (const auto &hit : sinkHits) {
    os << "  - ";
    if (hit.call && hit.call->getCalledFunction())
      os << hit.call->getCalledFunction()->getName();
    else
      os << "<indirect>";
    if (!hit.tainted_inputs.empty()) {
      os << " [";
      for (size_t i = 0; i < hit.tainted_inputs.size(); ++i) {
        if (i)
          os << ", ";
        os << hit.tainted_inputs[i];
      }
      os << "]";
    }
    os << "\n";
  }
}

InterTaint::Result InterTaint::run(llvm::Module &M,
                                   lotus::AliasAnalysisWrapper &aliasAnalysis,
                                   const Options &options, bool verbose,
                                   LinearStrategy linearStrategy) {
  bool configLoaded =
      options.taint_config_path.empty()
          ? taint_config::load_default_config()
          : taint_config::load_config(options.taint_config_path);
  if (!configLoaded) {
    llvm::errs() << "Error: Could not load taint configuration\n";
    InterTaint::Result res;
    res.status.configuration_error = true;
    res.status.approximated = true;
    res.status.summary_solve.converged = false;
    res.status.propagation_converged = false;
    res.status.overall_converged = false;
    return res;
  }

  LinearStrategy strategy = linearStrategy;
  if (strategy == LinearStrategy::TensorProduct) {
    strategy = LinearStrategy::SCC;
    if (verbose)
      llvm::errs() << "[npa-taint] tensor strategy is unsupported for "
                      "TaintTransformer; using SCC\n";
  }

  TaintAnalysis analysis(M, aliasAnalysis, options);
  if (analysis.hasUnsupportedSpecs() && options.fail_on_unsupported_specs) {
    InterTaint::Result res;
    res.status.unsupported_specs = true;
    res.status.approximated = true;
    res.status.summary_solve.converged = false;
    res.status.propagation_converged = false;
    res.status.overall_converged = false;
    return res;
  }
  auto engineResult = InterEngine<TaintTransformer, TaintAnalysis>::run(
      M, analysis, verbose, strategy, options.call_resolution_mode);

  InterTaint::Result res;
  res.status = engineResult.status;
  if (analysis.hasUnsupportedSpecs()) {
    res.status.unsupported_specs = true;
    res.status.approximated = true;
    res.status.overall_converged = false;
  }
  res.summaries.insert(engineResult.summaries.begin(),
                       engineResult.summaries.end());
  for (auto &kv : engineResult.blockEntryFacts)
    res.blockFacts[kv.first] = kv.second;
  res.valueBits = analysis.getValueBits();
  res.pointerMemoryBits = analysis.buildPointerMemoryBits();
  res.reachablePointerMemoryBits = analysis.buildReachablePointerMemoryBits();
  res.blockReachablePointerMemoryBits =
      analysis.buildFlowSensitiveReachablePointerMemoryBits();

  std::unordered_map<const llvm::Function *, TaintTransformer::value_type>
      summaryMap;
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
        if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I)) {
          auto callTransfer = analysis.buildCallTransfer(*call, summaryMap);
          llvm::APInt postFact =
              TaintTransformer::apply(callTransfer, currentFact);
          auto taintedInputs =
              analysis.triggeredSinkInputs(*call, currentFact, postFact);
          if (!taintedInputs.empty())
            res.sinkHits.push_back({call, std::move(taintedInputs)});
          currentFact = std::move(postFact);
        } else {
          auto transfer = analysis.buildNormalTransfer(I);
          currentFact = TaintTransformer::apply(transfer, currentFact);
        }
      }
      res.blockExitFacts[{&BB}] = currentFact;
    }
  }
  return res;
}

InterTaint::Result
InterTaint::run(llvm::Module &M, lotus::AliasAnalysisWrapper &aliasAnalysis,
                bool verbose, LinearStrategy linearStrategy,
                IndirectCallResolutionMode callResolutionMode) {
  Options options;
  options.call_resolution_mode = callResolutionMode;
  return run(M, aliasAnalysis, options, verbose, linearStrategy);
}

} // namespace npa
