#include "Analysis/Spectre/CacheSpecuAnalysis.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace spectre {

using namespace llvm;

namespace {

unsigned normalizeAlignment(unsigned alignment) {
  return alignment == 0 ? 1 : alignment;
}

} // namespace

void SpectreAnalysisResult::dump(raw_ostream &os) const {
  os << "Spectre findings: " << Findings.size() << "\n";
  os << "Architectural hits/misses: " << ArchitecturalHits << "/"
     << ArchitecturalMisses << "\n";
  os << "Speculative hits/misses: " << SpeculativeHits << "/"
     << SpeculativeMisses << "\n";
  for (const auto &finding : Findings) {
    os << "Branch ";
    if (finding.Branch) {
      finding.Branch->print(os);
    } else {
      os << "<null>";
    }
    os << " divergent=" << finding.HasDivergence << "\n";
  }
}

unsigned CacheModel::GetTySize(Type *ty) {
  if (ty == nullptr) {
    return 0;
  }
  if (ty->isIntegerTy()) {
    return std::max<unsigned>(1, ty->getIntegerBitWidth() / 8);
  }
  if (ty->isHalfTy()) {
    return 2;
  }
  if (ty->isFloatTy()) {
    return 4;
  }
  if (ty->isDoubleTy()) {
    return 8;
  }
  if (ty->isPointerTy()) {
    return ARCH_SIZE;
  }
  if (auto *arrayTy = dyn_cast<ArrayType>(ty)) {
    return arrayTy->getNumElements() * GetTySize(arrayTy->getElementType());
  }
  if (auto *vectorTy = dyn_cast<VectorType>(ty)) {
    if (vectorTy->getElementCount().isScalable()) {
      return 0;
    }
    return vectorTy->getElementCount().getFixedValue() *
           GetTySize(vectorTy->getElementType());
  }
  if (auto *structTy = dyn_cast<StructType>(ty)) {
    if (structTy->isOpaque()) {
      return 0;
    }
    unsigned size = 0;
    for (Type *elementTy : structTy->elements()) {
      size += GetTySize(elementTy);
    }
    return size;
  }
  return 0;
}

int CacheModel::GEPInstPos(GetElementPtrInst &I, unsigned &from, unsigned &to) {
  from = 0;
  to = 0;
  APInt offsetBits(64, 0, true);
  if (!I.accumulateConstantOffset(I.getModule()->getDataLayout(), offsetBits)) {
    Type *accessTy = I.getResultElementType();
    unsigned size = GetTySize(accessTy);
    to = size == 0 ? 0 : size - 1;
    return 0;
  }

  from = static_cast<unsigned>(offsetBits.getZExtValue());
  unsigned size = GetTySize(I.getResultElementType());
  to = size == 0 ? from : from + size - 1;
  return 1;
}

CacheModel::CacheModel(unsigned lineSize, unsigned lineNum, unsigned setNum,
                       bool must)
    : CacheLineNum(lineNum), CacheLineSize(lineSize), CacheSetNum(setNum),
      CacheLinesPerSet(setNum == 0 ? 0 : lineNum / setNum), MaxAddr(0),
      MustMod(must), HitCount(0), MissCount(0), SpecuHitCount(0),
      SpecuMissCount(0) {
  auto isPowerOf2 = [](unsigned x) -> bool { return x != 0 && (x & (x - 1)) == 0; };
  if (!(isPowerOf2(lineSize) && isPowerOf2(setNum) && setNum != 0 &&
        lineNum % setNum == 0)) {
    errs() << "Fatal: cache configuration invalid!\n";
  }
}

CacheModel::~CacheModel() = default;

void CacheModel::SetAges(const std::vector<unsigned> &ages) { Ages = ages; }

void CacheModel::SetVarsMap(const ValueMap<Value *, Var *> &vars) {
  Vars.clear();
  for (const auto &entry : vars) {
    Vars[entry.first] = entry.second;
  }
}

bool CacheModel::ConfigConsistent(const CacheModel *model) const {
  return model != nullptr && CacheLineNum == model->CacheLineNum &&
         CacheLineSize == model->CacheLineSize &&
         CacheLinesPerSet == model->CacheLinesPerSet &&
         CacheSetNum == model->CacheSetNum;
}

bool CacheModel::CacheConsistent(const CacheModel *model) const {
  if (!ConfigConsistent(model) || Vars.size() != model->Vars.size() ||
      Ages.size() != model->Ages.size()) {
    return false;
  }

  for (const auto &entry : Vars) {
    auto other = model->Vars.find(entry.first);
    if (other == model->Vars.end()) {
      return false;
    }

    const Var *lhs = entry.second;
    const Var *rhs = other->second;
    if (lhs->AgeIndex != rhs->AgeIndex || lhs->AgeSize != rhs->AgeSize) {
      return false;
    }
  }
  return true;
}

bool CacheModel::isVarPartiallyCached(const Var *var) const {
  for (unsigned i = var->AgeIndex; i < var->AgeIndex + var->AgeSize; ++i) {
    if (i < Ages.size() && Ages[i] < CacheLinesPerSet) {
      return true;
    }
  }
  return false;
}

unsigned CacheModel::LocateVar(Value *var, unsigned offset) const {
  auto it = Vars.find(var);
  if (it == Vars.end()) {
    return static_cast<unsigned>(-1);
  }
  return (it->second->AddrB + offset) / CacheLineSize;
}

unsigned CacheModel::GetAge(Value *var, unsigned offset) const {
  unsigned cacheLoc = LocateVar(var, offset);
  if (cacheLoc == static_cast<unsigned>(-1) || cacheLoc >= Ages.size()) {
    return CacheLinesPerSet;
  }
  return Ages[cacheLoc];
}

unsigned CacheModel::SetAge(Value *var, unsigned age, unsigned offset) {
  unsigned cacheLoc = LocateVar(var, offset);
  if (cacheLoc == static_cast<unsigned>(-1) || cacheLoc >= Ages.size()) {
    return static_cast<unsigned>(-1);
  }
  Ages[cacheLoc] = age;
  return age;
}

unsigned CacheModel::SetAge(Value *var, unsigned age, unsigned b, unsigned e) {
  auto it = Vars.find(var);
  if (it == Vars.end()) {
    return static_cast<unsigned>(-1);
  }
  for (unsigned offset = b; offset <= e; ++offset) {
    unsigned cacheLoc = LocateVar(var, offset);
    if (cacheLoc < Ages.size()) {
      Ages[cacheLoc] = age;
    }
  }
  return age;
}

unsigned CacheModel::AddVar(Value *var, Type *ty, unsigned alignment) {
  if (Vars.find(var) != Vars.end()) {
    return 1;
  }

  unsigned size = GetTySize(ty);
  if (size == 0) {
    return 1;
  }

  Var *newVar = new Var{};
  alignment = normalizeAlignment(alignment);

  if (MaxAddr % alignment != 0) {
    MaxAddr = (MaxAddr / alignment + 1) * alignment;
  }

  newVar->AddrB = MaxAddr;
  newVar->AddrE = MaxAddr + size - 1;
  MaxAddr += size;
  newVar->Val = var;
  newVar->ty = ty;
  newVar->alignment = alignment;

  unsigned lineB = newVar->AddrB / CacheLineSize;
  unsigned lineE = newVar->AddrE / CacheLineSize;
  newVar->AgeIndex = lineB;
  newVar->AgeSize = lineE - lineB + 1;
  newVar->LineB = lineB % CacheLineNum;
  newVar->LineE = lineE % CacheLineNum;

  while (Ages.size() <= lineE) {
    Ages.push_back(CacheLinesPerSet);
  }

  Vars[var] = newVar;
  return 0;
}

unsigned CacheModel::Access(Value *var, bool force) {
  if (!force) {
    return Access(var, static_cast<unsigned>(0));
  }

  auto it = Vars.find(var);
  if (it == Vars.end()) {
    return 1;
  }

  for (int i = static_cast<int>(it->second->AgeIndex + it->second->AgeSize) - 1;
       i >= static_cast<int>(it->second->AgeIndex); --i) {
    Ages[i] = 0;
  }
  return 0;
}

unsigned CacheModel::Access(Value *var, unsigned offset) {
  auto it = Vars.find(var);
  if (it == Vars.end()) {
    return 1;
  }

  unsigned addr = it->second->AddrB + offset;
  if (addr > it->second->AddrE) {
    return 2;
  }

  unsigned cacheLoc = addr / CacheLineSize;
  if (cacheLoc >= Ages.size()) {
    return static_cast<unsigned>(-1);
  }

  unsigned age = Ages[cacheLoc];
  unsigned setNum = cacheLoc % CacheSetNum;
  cacheRecord.insert(cacheLoc);

  for (unsigned i = setNum; i < Ages.size(); i += CacheSetNum) {
    if (i == cacheLoc) {
      Ages[i] = 0;
      continue;
    }
    if (Ages[i] <= age) {
      if (Ages[i] == age && MustMod) {
        continue;
      }
      if (Ages[i] < CacheLinesPerSet) {
        Ages[i]++;
      }
    }
  }

  return age < CacheLinesPerSet ? 1 : 0;
}

CacheModel *CacheModel::fork() const {
  CacheModel *copy =
      new CacheModel(CacheLineSize, CacheLineNum, CacheSetNum, MustMod);
  copy->SetMaxAddr(MaxAddr);
  copy->SetVarsMap(Vars);
  copy->SetAges(Ages);
  copy->HitCount = HitCount;
  copy->MissCount = MissCount;
  copy->SpecuHitCount = SpecuHitCount;
  copy->SpecuMissCount = SpecuMissCount;
  copy->cacheRecord = cacheRecord;
  return copy;
}

bool CacheModel::equal(const CacheModel *model) const {
  return CacheConsistent(model) && Ages == model->Ages &&
         HitCount == model->HitCount && MissCount == model->MissCount &&
         SpecuHitCount == model->SpecuHitCount &&
         SpecuMissCount == model->SpecuMissCount;
}

CacheModel *CacheModel::merge(CacheModel *mod) {
  if (mod == nullptr) {
    return this;
  }
  if (!ConfigConsistent(mod)) {
    return this;
  }

  for (const auto &entry : mod->Vars) {
    Value *val = entry.first;
    if (Vars.find(val) == Vars.end()) {
      AddVar(val, entry.second->ty, entry.second->alignment);
    }
    for (unsigned i = 0; i < entry.second->AgeSize; ++i) {
      unsigned lhsIndex = Vars[val]->AgeIndex + i;
      unsigned rhsIndex = entry.second->AgeIndex + i;
      if (lhsIndex >= Ages.size() || rhsIndex >= mod->Ages.size()) {
        continue;
      }
      Ages[lhsIndex] = std::max(Ages[lhsIndex], mod->Ages[rhsIndex]);
    }
  }

  for (auto line : mod->cacheRecord) {
    cacheRecord.insert(line);
  }

  HitCount = std::max(HitCount, mod->HitCount);
  MissCount = std::max(MissCount, mod->MissCount);
  SpecuHitCount = std::max(SpecuHitCount, mod->SpecuHitCount);
  SpecuMissCount = std::max(SpecuMissCount, mod->SpecuMissCount);
  return this;
}

bool CacheModel::widenFrom(const CacheModel &previous) {
  bool changed = false;
  if (!ConfigConsistent(&previous)) {
    return false;
  }

  if (Ages.size() < previous.Ages.size()) {
    Ages.resize(previous.Ages.size(), CacheLinesPerSet);
    changed = true;
  }

  for (size_t i = 0; i < previous.Ages.size(); ++i) {
    unsigned next = Ages[i];
    if (next > previous.Ages[i]) {
      next = CacheLinesPerSet;
    }
    if (Ages[i] != next) {
      Ages[i] = next;
      changed = true;
    }
  }
  return changed;
}

void CacheModel::invalidateAll() {
  for (unsigned &age : Ages) {
    age = CacheLinesPerSet;
  }
}

bool CacheModel::isInCache(const std::string &varName) const {
  for (const auto &entry : Vars) {
    if (entry.first->getName() == varName && isVarPartiallyCached(entry.second)) {
      return Ages[entry.second->AgeIndex] < CacheLinesPerSet;
    }
  }
  return false;
}

void CacheModel::dump(bool verbose) const {
  dbgs() << "======== cache state ========\n";
  for (const auto &entry : Vars) {
    const Var *var = entry.second;
    if (!verbose && !isVarPartiallyCached(var)) {
      continue;
    }
    if (entry.first->hasName()) {
      dbgs() << entry.first->getName() << "\n";
    } else {
      entry.first->print(dbgs());
      dbgs() << "\n";
    }
    if (verbose) {
      dbgs() << "\taddrB: " << var->AddrB << " addrE: " << var->AddrE
             << " ageIndex: " << var->AgeIndex
             << " ageSize: " << var->AgeSize
             << " align: " << var->alignment << "\n";
    }
    dbgs() << "\tages: ";
    for (unsigned i = var->AgeIndex; i < var->AgeIndex + var->AgeSize; ++i) {
      dbgs() << Ages[i] << " ";
    }
    dbgs() << "\n";
  }
  dbgs() << "hits=" << HitCount << " misses=" << MissCount
         << " specu_hits=" << SpecuHitCount
         << " specu_misses=" << SpecuMissCount << "\n";
}

} // namespace spectre
