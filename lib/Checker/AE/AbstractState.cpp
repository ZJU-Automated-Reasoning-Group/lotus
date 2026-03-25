//===- AbstractState.cpp -- Abstract State--------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/AbstractState.h"

#include "Alias/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSObject.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/MemBlock.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/AE/SVFIRWrapper.h"

#include <algorithm>

#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace analysis {

namespace {

int64_t clampFieldOffset(int64_t offset) {
  if (offset < -static_cast<int64_t>(MaxFieldLimit))
    return -static_cast<int64_t>(MaxFieldLimit);
  if (offset > static_cast<int64_t>(MaxFieldLimit))
    return static_cast<int64_t>(MaxFieldLimit);
  return offset;
}

int64_t clampFieldOffset(const BoundedInt &bound) {
  if (bound.is_minus_infinity())
    return -static_cast<int64_t>(MaxFieldLimit);
  if (bound.is_plus_infinity())
    return static_cast<int64_t>(MaxFieldLimit);
  return clampFieldOffset(bound.getIntNumeral());
}

std::pair<uint32_t, int64_t>
resolvePointerBaseAndOffset(AbstractState &as, const llvm::Value *value,
                            uint32_t fallbackId) {
  const llvm::Value *base = value;
  int64_t byteOffset = 0;

  while (base) {
    if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(base)) {
      int64_t step = clampFieldOffset(as.getByteOffset(gep).ub());
      byteOffset = clampFieldOffset(byteOffset + step);
      base = gep->getPointerOperand();
      continue;
    }
    if (const auto *op = llvm::dyn_cast<llvm::Operator>(base)) {
      if (op->getOpcode() == llvm::Instruction::BitCast ||
          op->getOpcode() == llvm::Instruction::AddrSpaceCast) {
        base = op->getOperand(0);
        continue;
      }
    }
    break;
  }

  uint32_t baseId =
      base ? AbstractInterpretation::getValueIdStatic(base) : fallbackId;
  return std::make_pair(baseId, byteOffset);
}

} // namespace

/// Widen interval to ensure termination of analysis
AbstractState AbstractState::widening(const AbstractState &other) {
  // Match SVF semantics: iterate over existing keys in this, then widen with
  // other
  AbstractState result = *this;
  for (auto it = result._varToAbsVal.begin(); it != result._varToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._varToAbsVal.find(key) != other._varToAbsVal.end()) {
      if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval()) {
        it->second.getInterval().widen_with(
            other._varToAbsVal.at(key).getInterval());
      }
    }
  }
  for (auto it = result._addrToAbsVal.begin(); it != result._addrToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end()) {
      if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval()) {
        it->second.getInterval().widen_with(
            other._addrToAbsVal.at(key).getInterval());
      }
    }
  }
  // Preserve may-freed facts monotonically.
  result._freedAddrs.insert(other._freedAddrs.begin(), other._freedAddrs.end());
  result._heapObjs.insert(other._heapObjs.begin(), other._heapObjs.end());
  // Conservatively keep the smallest known object size per object so potential
  // OOB is not hidden on merges.
  for (const auto &entry : other._objToSize) {
    auto it = result._objToSize.find(entry.first);
    if (it == result._objToSize.end()) {
      result._objToSize.emplace(entry.first, entry.second);
    } else {
      it->second = std::min(it->second, entry.second);
    }
  }
  return result;
}

/// Narrow interval to refine over-approximation from widening
AbstractState AbstractState::narrowing(const AbstractState &other) {
  // Match SVF semantics: iterate over existing keys in this, then narrow with
  // other
  AbstractState result = *this;
  for (auto it = result._varToAbsVal.begin(); it != result._varToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._varToAbsVal.find(key) != other._varToAbsVal.end()) {
      if (it->second.isInterval() && other._varToAbsVal.at(key).isInterval()) {
        it->second.getInterval().narrow_with(
            other._varToAbsVal.at(key).getInterval());
      }
    }
  }
  for (auto it = result._addrToAbsVal.begin(); it != result._addrToAbsVal.end();
       ++it) {
    auto key = it->first;
    if (other._addrToAbsVal.find(key) != other._addrToAbsVal.end()) {
      if (it->second.isInterval() && other._addrToAbsVal.at(key).isInterval()) {
        it->second.getInterval().narrow_with(
            other._addrToAbsVal.at(key).getInterval());
      }
    }
  }
  // Narrowing keeps may-freed facts monotone.
  result._freedAddrs.insert(other._freedAddrs.begin(), other._freedAddrs.end());
  result._heapObjs.insert(other._heapObjs.begin(), other._heapObjs.end());
  // Keep conservative (smallest) known object size.
  for (const auto &entry : other._objToSize) {
    auto it = result._objToSize.find(entry.first);
    if (it == result._objToSize.end()) {
      result._objToSize.emplace(entry.first, entry.second);
    } else {
      it->second = std::min(it->second, entry.second);
    }
  }
  return result;
}

/// Domain join with other, important! other widen this.
void AbstractState::joinWith(const AbstractState &other) {
  // Match SVF semantics: only join with existing keys, otherwise emplace
  for (auto it = other._varToAbsVal.begin(); it != other._varToAbsVal.end();
       ++it) {
    auto key = it->first;
    auto oit = _varToAbsVal.find(key);
    if (oit != _varToAbsVal.end()) {
      oit->second.join_with(it->second);
    } else {
      _varToAbsVal.emplace(key, it->second);
    }
  }
  for (auto it = other._addrToAbsVal.begin(); it != other._addrToAbsVal.end();
       ++it) {
    auto key = it->first;
    auto oit = _addrToAbsVal.find(key);
    if (oit != _addrToAbsVal.end()) {
      oit->second.join_with(it->second);
    } else {
      _addrToAbsVal.emplace(key, it->second);
    }
  }
  // Union of freed addresses (matching SVF)
  _freedAddrs.insert(other._freedAddrs.begin(), other._freedAddrs.end());
  _heapObjs.insert(other._heapObjs.begin(), other._heapObjs.end());
  // Merge object sizes conservatively (min size surfaces potential OOB).
  for (const auto &entry : other._objToSize) {
    auto it = _objToSize.find(entry.first);
    if (it == _objToSize.end()) {
      _objToSize.emplace(entry.first, entry.second);
    } else {
      it->second = std::min(it->second, entry.second);
    }
  }
}

/// Domain meet with other, important! other widen this.
void AbstractState::meetWith(const AbstractState &other) {
  for (const auto &item : other._varToAbsVal) {
    auto it = _varToAbsVal.find(item.first);
    if (it != _varToAbsVal.end()) {
      it->second.meet_with(item.second);
    }
  }
  for (const auto &item : other._addrToAbsVal) {
    auto it = _addrToAbsVal.find(item.first);
    if (it != _addrToAbsVal.end()) {
      it->second.meet_with(item.second);
    }
  }
  // Compute intersection of freed addresses
  std::unordered_set<uint32_t> intersection;
  for (uint32_t addr : _freedAddrs) {
    if (other._freedAddrs.find(addr) != other._freedAddrs.end()) {
      intersection.insert(addr);
    }
  }
  _freedAddrs = std::move(intersection);
  std::unordered_set<uint32_t> heapIntersection;
  for (uint32_t objId : _heapObjs) {
    if (other._heapObjs.find(objId) != other._heapObjs.end()) {
      heapIntersection.insert(objId);
    }
  }
  _heapObjs = std::move(heapIntersection);
  // Meet object sizes by requiring presence in both and taking max.
  std::unordered_map<uint32_t, uint32_t> objIntersection;
  for (const auto &entry : _objToSize) {
    auto it = other._objToSize.find(entry.first);
    if (it != other._objToSize.end()) {
      objIntersection.emplace(entry.first, std::max(entry.second, it->second));
    }
  }
  _objToSize = std::move(objIntersection);
}

void AbstractState::printAbstractState() const {
  llvm::outs() << "Abstract State:\n";
  llvm::outs() << "  Variables:\n";
  for (const auto &item : _varToAbsVal) {
    llvm::outs() << "    " << item.first << " -> " << item.second.toString()
                 << "\n";
  }
  llvm::outs() << "  Memory:\n";
  for (const auto &item : _addrToAbsVal) {
    llvm::outs() << "    " << item.first << " -> " << item.second.toString()
                 << "\n";
  }
}

bool AbstractState::equals(const AbstractState &other) const {
  return eqVarToValMap(_varToAbsVal, other._varToAbsVal) &&
         eqVarToValMap(_addrToAbsVal, other._addrToAbsVal) &&
         _freedAddrs == other._freedAddrs && _heapObjs == other._heapObjs &&
         _objToSize == other._objToSize;
}

uint32_t AbstractState::hash() const {
  // Szudzik's pairing function from SVF - elegant way to combine two hash
  // values
  auto szudzik = [](size_t a, size_t b) -> size_t {
    return a > b ? b * b + a : a * a + a + b;
  };

  std::hash<uint32_t> hf;
  size_t h = szudzik(getVarToVal().size() * 2, 0);
  for (const auto &t : getVarToVal()) {
    h = szudzik(h, hf(t.first));
  }
  size_t h2 = szudzik(getLocToVal().size() * 2, 0);
  for (const auto &t : getLocToVal()) {
    h2 = szudzik(h2, hf(t.first));
  }
  // Combine both hashes using Szudzik
  size_t h3 = szudzik(_freedAddrs.size() * 2, 0);
  for (const auto &addr : _freedAddrs) {
    h3 = szudzik(h3, hf(addr));
  }
  size_t h4 = szudzik(_heapObjs.size() * 2, 0);
  for (const auto &objId : _heapObjs) {
    h4 = szudzik(h4, hf(objId));
  }
  size_t h5 = szudzik(_objToSize.size() * 2, 0);
  for (const auto &item : _objToSize) {
    h5 = szudzik(h5, szudzik(hf(item.first), hf(item.second)));
  }
  return static_cast<uint32_t>(
      szudzik(szudzik(h, h2), szudzik(szudzik(h3, h4), h5)));
}

AbstractValue AbstractState::loadValue(uint32_t varId) {
  AbstractValue result;

  // Load from addresses in the abstract state
  if (inVarToAddrsTable(varId)) {
    for (auto addr : _varToAbsVal[varId].getAddrs()) {
      if (!AddressValue::isVirtualMemAddress(addr))
        continue;
      if (isFreedMem(addr)) {
        result = AbstractValue(IntervalValue::bottom());
        continue;
      }
      result.join_with(load(addr));
    }
  }

  // Resolve field-sensitive loads through the original base pointer when the
  // pointer value is a GEP/cast chain and the direct address-set lookup did not
  // recover a value.
  if (result.getAddrs().isBottom() && result.getInterval().isBottom()) {
    const llvm::Value *ptrVal =
        AbstractInterpretation::getAEInstance().getValueFromIdStatic(varId);
    if (ptrVal && ptrVal->getType()->isPointerTy()) {
      std::pair<uint32_t, int64_t> resolved =
          resolvePointerBaseAndOffset(*this, ptrVal, varId);
      AddressValue derivedAddrs =
          getGepObjAddrs(resolved.first, IntervalValue(resolved.second));
      for (uint32_t addr : derivedAddrs) {
        if (!AddressValue::isVirtualMemAddress(addr) || isFreedMem(addr))
          continue;
        uint32_t objId = getIDFromAddr(addr);
        if (inAddrToValTable(objId) || inAddrToAddrsTable(objId))
          result.join_with(load(addr));
      }
    }
  }

  // Use PTA to get more precise points-to information (matching SVF's behavior)
  if (svfir_ && svfir_->isPTAReady()) {
    const llvm::Value *val =
        AbstractInterpretation::getAEInstance().getValueFromIdStatic(varId);
    if (val && val->getType()->isPointerTy()) {
      std::vector<void *> pts;
      svfir_->getPointsTo(val, pts);

      using FSObject = aser::FSObject<aser::NoCtx>;
      for (void *obj : pts) {
        if (!obj)
          continue;
        auto *fsObj = static_cast<const FSObject *>(obj);
        const llvm::Value *objVal = fsObj->getValue();
        if (!objVal)
          continue;
        uint32_t objId = AbstractInterpretation::getValueIdStatic(objVal);
        if (objId == 0) // ConstantPointerNull; getVirtualMemAddress(0) asserts
          continue;
        uint32_t objAddr = getVirtualMemAddress(objId);
        if (!isNullMem(objAddr) && !isFreedMem(objAddr)) {
          if (inAddrToValTable(objId)) {
            result.join_with(load(objAddr));
          }
        }
      }
    }
  }

  return result;
}

void AbstractState::storeValue(uint32_t varId, uint32_t valId) {
  AbstractValue val = _varToAbsVal[valId];

  // Store to addresses in abstract state
  if (inVarToAddrsTable(varId)) {
    for (auto addr : _varToAbsVal[varId].getAddrs()) {
      if (!AddressValue::isVirtualMemAddress(addr) || isFreedMem(addr))
        continue;
      store(addr, val);
    }
  }

  const llvm::Value *valPtr =
      AbstractInterpretation::getAEInstance().getValueFromIdStatic(varId);
  if (valPtr && valPtr->getType()->isPointerTy()) {
    std::pair<uint32_t, int64_t> resolved =
        resolvePointerBaseAndOffset(*this, valPtr, varId);
    AddressValue derivedAddrs =
        getGepObjAddrs(resolved.first, IntervalValue(resolved.second));
    for (uint32_t addr : derivedAddrs) {
      if (!AddressValue::isVirtualMemAddress(addr) || isFreedMem(addr))
        continue;
      store(addr, val);
    }
  }

  // Use PTA to get more precise points-to information (matching SVF's behavior)
  if (svfir_ && svfir_->isPTAReady()) {
    const llvm::Value *valPtr =
        AbstractInterpretation::getAEInstance().getValueFromIdStatic(varId);
    if (valPtr && valPtr->getType()->isPointerTy()) {
      std::vector<void *> pts;
      svfir_->getPointsTo(valPtr, pts);

      using FSObject = aser::FSObject<aser::NoCtx>;
      for (void *obj : pts) {
        if (!obj)
          continue;
        auto *fsObj = static_cast<const FSObject *>(obj);
        const llvm::Value *objVal = fsObj->getValue();
        if (!objVal)
          continue;
        uint32_t objId = AbstractInterpretation::getValueIdStatic(objVal);
        if (objId == 0) // ConstantPointerNull; getVirtualMemAddress(0) asserts
          continue;
        uint32_t objAddr = getVirtualMemAddress(objId);
        if (!isNullMem(objAddr) && !isFreedMem(objAddr)) {
          store(objAddr, val);
        }
      }
    }
  }
}

AddressValue AbstractState::getGepObjAddrs(uint32_t pointer,
                                           IntervalValue offset,
                                           const llvm::GetElementPtrInst *gep) {
  if (gep) {
    setGepObjOffsetFromBase(gep, offset);
  }
  return getGepObjAddrs(pointer, offset);
}

AddressValue AbstractState::getGepObjAddrs(uint32_t pointer,
                                           IntervalValue offset) {
  AddressValue result;
  if (!inVarToAddrsTable(pointer))
    return result;

  // Preserve finite negative offsets while still bounding infinities and very
  // large intervals to keep field-object expansion finite.
  int64_t lb = clampFieldOffset(offset.lb());
  int64_t ub = clampFieldOffset(offset.ub());

  for (auto addr : _varToAbsVal[pointer].getAddrs()) {
    uint32_t baseObjId = getIDFromAddr(addr);

    // Handle offset interval - create field-sensitive GEP nodes similar to SVF
    if (offset.is_numeral()) {
      int64_t offsetVal = clampFieldOffset(offset.getIntNumeral());
      // Create field-sensitive GEP object ID (matching SVF's getGepObjVar)
      uint32_t gepObjId = getGepFieldObjId(baseObjId, offsetVal);
      uint32_t gepAddr = getVirtualMemAddress(gepObjId);
      uint32_t objSize = getObjSize(baseObjId);
      if (objSize > 0) {
        setObjSize(gepObjId, objSize);
      }
      result.insert(gepAddr);
    } else {
      // For interval offset, iterate over the range (limited to MaxFieldLimit)
      for (int64_t i = lb; i <= ub; ++i) {
        // Create field-sensitive GEP object ID for each offset
        uint32_t gepObjId = getGepFieldObjId(baseObjId, i);
        uint32_t gepAddr = getVirtualMemAddress(gepObjId);
        uint32_t objSize = getObjSize(baseObjId);
        if (objSize > 0) {
          setObjSize(gepObjId, objSize);
        }
        result.insert(gepAddr);
      }
    }
  }
  return result;
}

IntervalValue AbstractState::getByteOffset(const llvm::GetElementPtrInst *gep) {
  const llvm::DataLayout &dl = gep->getModule()->getDataLayout();
  llvm::Type *curType = gep->getSourceElementType();
  IntervalValue offset(0);
  bool firstIndex = true;

  for (const auto *idxIt = gep->idx_begin(); idxIt != gep->idx_end(); ++idxIt) {
    llvm::Value *idx = *idxIt;

    // Get index value
    IntervalValue idxVal(0);
    if (llvm::ConstantInt *cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
      idxVal = IntervalValue(cidx->getSExtValue());
    } else {
      // Use stable ID from AbstractInterpretation
      uint32_t idxId = AbstractInterpretation::getValueIdStatic(idx);
      if (inVarToValTable(idxId)) {
        idxVal = _varToAbsVal[idxId].getInterval();
      }
    }

    if (firstIndex) {
      if (!curType || !curType->isSized()) {
        return IntervalValue(0, MaxFieldLimit);
      }

      uint64_t elemSize = dl.getTypeAllocSize(curType);
      if (elemSize == 0) {
        elemSize = 1;
      }
      if (elemSize > static_cast<uint64_t>(MaxFieldLimit)) {
        elemSize = MaxFieldLimit;
      }

      offset = offset + idxVal * IntervalValue(static_cast<int64_t>(elemSize));
      firstIndex = false;
      continue;
    }

    // Struct indexing: only constant field indices are valid in LLVM IR.
    if (auto *structType = llvm::dyn_cast<llvm::StructType>(curType)) {
      if (!structType->isSized()) {
        return IntervalValue(0, MaxFieldLimit);
      }
      if (!idxVal.is_numeral()) {
        return IntervalValue(0, MaxFieldLimit);
      }

      if (idxVal.is_numeral()) {
        int64_t idxNum = idxVal.getIntNumeral();
        if (idxNum >= 0 &&
            idxNum < static_cast<int64_t>(structType->getNumElements())) {
          const llvm::StructLayout *layout = dl.getStructLayout(structType);
          uint32_t fieldOffset =
              layout->getElementOffset(static_cast<unsigned>(idxNum));
          offset = offset + IntervalValue(fieldOffset);
          curType = structType->getElementType(static_cast<unsigned>(idxNum));
          continue;
        }
        return IntervalValue(0, MaxFieldLimit);
      }
    }

    llvm::Type *elemType = nullptr;
    if (auto *arrType = llvm::dyn_cast<llvm::ArrayType>(curType)) {
      elemType = arrType->getElementType();
    } else if (auto *vecType = llvm::dyn_cast<llvm::VectorType>(curType)) {
      elemType = vecType->getElementType();
    } else if (curType->isPointerTy()) {
      elemType = curType->getPointerElementType();
    }

    if (!elemType) {
      break;
    }
    if (!elemType->isSized()) {
      return IntervalValue(0, MaxFieldLimit);
    }

    uint64_t elemSize = dl.getTypeAllocSize(elemType);
    if (elemSize == 0) {
      elemSize = 1;
    }
    if (elemSize > static_cast<uint64_t>(MaxFieldLimit)) {
      elemSize = MaxFieldLimit;
    }

    offset = offset + idxVal * IntervalValue(static_cast<int64_t>(elemSize));
    curType = elemType;
  }

  return offset;
}

uint32_t AbstractState::getAllocaInstByteSize(const llvm::AllocaInst *alloca) {
  llvm::Type *allocType = alloca->getAllocatedType();
  const llvm::DataLayout &dl = alloca->getModule()->getDataLayout();
  uint32_t typeSize = dl.getTypeAllocSize(allocType);

  // Try to get more accurate size from PTA first
  if (svfir_ && svfir_->isPTAReady()) {
    uint32_t ptaSize = getObjectSize(alloca);
    if (ptaSize > 0) {
      return ptaSize;
    }
  }

  // Handle array allocation
  if (alloca->isArrayAllocation()) {
    const llvm::Value *arraySize = alloca->getArraySize();
    if (const llvm::ConstantInt *csize =
            llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
      return typeSize * static_cast<uint32_t>(csize->getZExtValue());
    } else {
      // Variable-sized array - use MaxFieldLimit as upper bound
      // This matches SVF's behavior for non-constant array sizes
      return typeSize * MaxFieldLimit;
    }
  }

  return typeSize;
}

uint32_t AbstractState::getAllocaInstByteSize(const llvm::AllocaInst *alloca,
                                              const AbstractState &as) {
  llvm::Type *allocType = alloca->getAllocatedType();
  const llvm::DataLayout &dl = alloca->getModule()->getDataLayout();
  uint32_t typeSize = dl.getTypeAllocSize(allocType);

  // Try to get more accurate size from PTA first
  if (svfir_ && svfir_->isPTAReady()) {
    uint32_t ptaSize = getObjectSize(alloca);
    if (ptaSize > 0) {
      return ptaSize;
    }
  }

  // Handle array allocation
  if (alloca->isArrayAllocation()) {
    const llvm::Value *arraySize = alloca->getArraySize();
    if (const llvm::ConstantInt *csize =
            llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
      // Constant array size
      return typeSize * static_cast<uint32_t>(csize->getZExtValue());
    } else {
      // Variable-sized array - try to get size from abstract state
      uint32_t arraySizeId =
          AbstractInterpretation::getValueIdStatic(arraySize);

      if (as.inVarToValTable(arraySizeId)) {
        // Array size is tracked in abstract state
        IntervalValue sizeInterval = as[arraySizeId].getInterval();

        if (!sizeInterval.isBottom() && !sizeInterval.isTop()) {
          // Use upper bound of interval, clamped to MaxFieldLimit
          int64_t ub = sizeInterval.ub().getIntNumeral();
          if (ub < 0) {
            ub = 0;
          }
          if (ub > static_cast<int64_t>(MaxFieldLimit)) {
            ub = MaxFieldLimit;
          }
          // Default element size is 1 (matching SVF's behavior)
          uint32_t elementSize = typeSize > 0 ? typeSize : 1;
          uint64_t res =
              static_cast<uint64_t>(elementSize) * static_cast<uint64_t>(ub);
          // Clamp result to MaxFieldLimit if needed
          if (res > MaxFieldLimit) {
            res = MaxFieldLimit;
          }
          return static_cast<uint32_t>(res);
        }
      }

      // Fallback: use MaxFieldLimit as conservative upper bound
      // This happens when array size is not tracked in abstract state yet
      return typeSize * MaxFieldLimit;
    }
  }

  return typeSize;
}

void AbstractState::initObjVar(const llvm::Value *objVar) {
  // Use stable ID from AbstractInterpretation
  uint32_t varId = AbstractInterpretation::getValueIdStatic(objVar);
  uint32_t objId = varId; // For memory objects, objId == varId

  // Check if it's a global variable
  if (const llvm::GlobalVariable *gv =
          llvm::dyn_cast<llvm::GlobalVariable>(objVar)) {
    if (gv->hasInitializer()) {
      if (const llvm::ConstantInt *ci =
              llvm::dyn_cast<llvm::ConstantInt>(gv->getInitializer())) {
        (*this)[varId] = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
        return;
      } else if (const llvm::ConstantFP *cfp =
                     llvm::dyn_cast<llvm::ConstantFP>(gv->getInitializer())) {
        double val = cfp->getValueAPF().convertToDouble();
        (*this)[varId] = IntervalValue(val, val);
        return;
      } else if (gv->getInitializer()->isNullValue()) {
        (*this)[varId] =
            IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
        return;
      }
    }
    // Global pointer or complex type - track size
    if (gv->getValueType()->isPointerTy()) {
      llvm::Type *pointeeType = gv->getValueType()->getPointerElementType();
      if (pointeeType) {
        const llvm::DataLayout &dl = gv->getParent()->getDataLayout();
        uint32_t size = dl.getTypeAllocSize(pointeeType);
        setObjSize(objId, size);
      }
    }
    (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
    return;
  }

  // Check if it's an alloca instruction
  if (const llvm::AllocaInst *alloca =
          llvm::dyn_cast<llvm::AllocaInst>(objVar)) {
    uint32_t size = getAllocaInstByteSize(alloca);
    setObjSize(objId, size);
    (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
    return;
  }

  // Check if it's a constant
  if (const llvm::ConstantInt *ci = llvm::dyn_cast<llvm::ConstantInt>(objVar)) {
    (*this)[varId] = IntervalValue(ci->getSExtValue(), ci->getSExtValue());
    return;
  }
  if (const llvm::ConstantFP *cfp = llvm::dyn_cast<llvm::ConstantFP>(objVar)) {
    double val = cfp->getValueAPF().convertToDouble();
    (*this)[varId] = IntervalValue(val, val);
    return;
  }
  if (llvm::isa<llvm::ConstantPointerNull>(objVar)) {
    (*this)[varId] =
        IntervalValue(static_cast<int64_t>(0), static_cast<int64_t>(0));
    return;
  }

  // For constant arrays/structs, use top
  if (llvm::isa<llvm::ConstantArray>(objVar) ||
      llvm::isa<llvm::ConstantStruct>(objVar)) {
    (*this)[varId] = IntervalValue::top();
    return;
  }

  // Default: treat as memory object with virtual address
  // Try to determine size from type if it's a pointer
  if (objVar->getType()->isPointerTy()) {
    llvm::Type *pointeeType = objVar->getType()->getPointerElementType();
    if (pointeeType && llvm::isa<llvm::Instruction>(objVar)) {
      const llvm::Instruction *inst = llvm::cast<llvm::Instruction>(objVar);
      if (inst->getModule()) {
        const llvm::DataLayout &dl = inst->getModule()->getDataLayout();
        uint32_t size = dl.getTypeAllocSize(pointeeType);
        setObjSize(objId, size);
      }
    }
  }
  (*this)[varId] = AddressValue(getVirtualMemAddress(varId));
}

IntervalValue
AbstractState::getElementIndex(const llvm::GetElementPtrInst *gep) {
  // Check if GEP has constant offset
  if (gep->hasAllConstantIndices()) {
    llvm::APInt offset(64, 0);
    if (gep->accumulateConstantOffset(gep->getModule()->getDataLayout(),
                                      offset)) {
      return IntervalValue(offset.getSExtValue(), offset.getSExtValue());
    }
  }

  IntervalValue res(0);
  llvm::Type *srcType = gep->getSourceElementType();
  const llvm::DataLayout &dl = gep->getModule()->getDataLayout();

  // Iterate over indices in reverse order (matching SVF's behavior)
  for (int i = gep->getNumIndices() - 1; i >= 0; --i) {
    llvm::Value *idx =
        gep->getOperand(i + 1); // +1 because operand 0 is pointer
    llvm::Type *idxType = srcType;

    int64_t idxLb, idxUb;

    // Get index value bounds
    if (llvm::ConstantInt *cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
      idxLb = idxUb = cidx->getSExtValue();
    } else {
      // Use stable ID from AbstractInterpretation
      uint32_t idxId = AbstractInterpretation::getValueIdStatic(idx);
      if (inVarToValTable(idxId)) {
        IntervalValue idxItv = (*this)[idxId].getInterval();
        if (idxItv.isBottom()) {
          idxLb = idxUb = 0;
        } else if (idxItv.is_infinite()) {
          idxLb = 0;
          idxUb = MaxFieldLimit;
        } else {
          idxLb = idxItv.lb().getIntNumeral();
          idxUb = idxItv.ub().getIntNumeral();
        }
      } else {
        idxLb = 0;
        idxUb = MaxFieldLimit;
      }
    }

    // Adjust bounds based on type
    if (idxType->isPointerTy()) {
      llvm::Type *pointeeType = idxType->getPointerElementType();
      if (!pointeeType || !pointeeType->isSized()) {
        // Opaque/unsized type: return conservative [0, MaxFieldLimit]
        res.meet_with(IntervalValue(static_cast<int64_t>(0),
                                    static_cast<int64_t>(MaxFieldLimit)));
        if (res.isBottom())
          res = IntervalValue(static_cast<int64_t>(0));
        return res;
      }
      uint32_t elemSize = dl.getTypeAllocSize(pointeeType);
      if (elemSize == 0)
        elemSize = 1;
      uint32_t elemNum = MaxFieldLimit / elemSize;
      if (idxLb > static_cast<int64_t>(elemNum))
        idxLb = MaxFieldLimit;
      else
        idxLb *= elemSize;
      if (idxUb > static_cast<int64_t>(elemNum))
        idxUb = MaxFieldLimit;
      else
        idxUb *= elemSize;
    } else if (idxType->isArrayTy()) {
      uint32_t arraySize = idxType->getArrayNumElements();
      if (idxUb >= static_cast<int64_t>(arraySize) || idxLb < 0) {
        idxLb = idxUb = 0;
      }
      // For arrays, element index is just the index value
    } else if (idxType->isStructTy()) {
      llvm::StructType *structType = llvm::cast<llvm::StructType>(idxType);
      uint32_t numElements = structType->getNumElements();
      if (idxUb >= static_cast<int64_t>(numElements) || idxLb < 0) {
        idxLb = idxUb = 0;
      }
      // For structs, element index is just the field index
    }

    res = res + IntervalValue(idxLb, idxUb);

    // Update srcType for next iteration
    if (idxType->isArrayTy()) {
      srcType = idxType->getArrayElementType();
    } else if (idxType->isStructTy() && idxLb == idxUb && idxLb >= 0) {
      llvm::StructType *structType = llvm::cast<llvm::StructType>(idxType);
      if (static_cast<uint32_t>(idxLb) < structType->getNumElements()) {
        srcType = structType->getElementType(static_cast<unsigned>(idxLb));
      }
    } else if (idxType->isPointerTy()) {
      srcType = idxType->getPointerElementType();
    }
  }

  // Ensure result is within [0, MaxFieldLimit]
  res.meet_with(IntervalValue(static_cast<int64_t>(0),
                              static_cast<int64_t>(MaxFieldLimit)));
  if (res.isBottom()) {
    res = IntervalValue(static_cast<int64_t>(0));
  }
  return res;
}

const llvm::Type *AbstractState::getPointeeElement(uint32_t id) {
  if (inVarToAddrsTable(id)) {
    const AbstractValue &addrs = (*this)[id];
    const AddressValue &addrVal = addrs.getAddrs();
    for (auto addr : addrVal) {
      uint32_t addr_id = getIDFromAddr(addr);
      if (addr_id == 0) // nullptr skip
        continue;

      // Get the LLVM Value from AbstractInterpretation's mapping
      // Use singleton instance to avoid circular dependency
      // Use fully qualified name to avoid shadowing by forward declaration in
      // AbstractState.h
      const llvm::Value *val =
          lotus::analysis::AbstractInterpretation::getAEInstance()
              .getValueFromIdStatic(addr_id);
      if (!val)
        continue;

      // Get the pointee type from LLVM's type system
      llvm::Type *type = val->getType();
      if (type && type->isPointerTy()) {
        return type->getPointerElementType();
      }

      // If val is an AllocaInst, get the allocated type
      if (const llvm::AllocaInst *alloca =
              llvm::dyn_cast<llvm::AllocaInst>(val)) {
        return alloca->getAllocatedType();
      }

      // If val is a GlobalVariable, get the value type
      if (const llvm::GlobalVariable *gv =
              llvm::dyn_cast<llvm::GlobalVariable>(val)) {
        return gv->getValueType();
      }
    }
  }
  return nullptr;
}

uint32_t AbstractState::getObjectSize(const llvm::Value *obj) const {
  if (!svfir_ || !svfir_->isPTAReady())
    return 0;

  if (!obj || !obj->getType()->isPointerTy())
    return 0;

  // Match SVF semantics: size belongs to the resolved base object, not merely
  // to the queried pointer's pointee type.
  std::vector<void *> pts;
  svfir_->getPointsTo(obj, pts);

  uint32_t bestSize = 0;
  for (void *target : pts) {
    uint32_t targetSize = svfir_->getByteSizeOfObj(target);
    if (targetSize == 0)
      continue;
    if (bestSize == 0 || targetSize < bestSize) {
      bestSize = targetSize;
    }
  }

  if (bestSize != 0) {
    return bestSize;
  }

  const llvm::Type *pointeeType = svfir_->getObjectType(obj);
  if (pointeeType) {
    llvm::Module *mod = svfir_->getModule();
    if (mod && pointeeType->isSized()) {
      const llvm::DataLayout &dl = mod->getDataLayout();
      return dl.getTypeAllocSize(const_cast<llvm::Type *>(pointeeType));
    }
  }
  return 0;
}

void AbstractState::getPointsToSet(const llvm::Value *ptr,
                                   std::vector<void *> &result) const {
  if (svfir_ && svfir_->isPTAReady()) {
    svfir_->getPointsTo(ptr, result);
  } else {
    result.clear();
  }
}

} // namespace analysis
} // namespace lotus
