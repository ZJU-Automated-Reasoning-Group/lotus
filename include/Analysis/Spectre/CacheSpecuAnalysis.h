/**
 * @file CacheSpecuAnalysis.h
 * @brief Spectre-v1-style cache side-channel analysis.
 */

#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace spectre {

using llvm::AliasAnalysis;
using llvm::BasicBlock;
using llvm::BranchInst;
using llvm::CallInst;
using llvm::ConstantExpr;
using llvm::DominatorTree;
using llvm::Function;
using llvm::GetElementPtrInst;
using llvm::GlobalVariable;
using llvm::Instruction;
using llvm::IntrinsicInst;
using llvm::PHINode;
using llvm::PostDominatorTree;
using llvm::SelectInst;
using llvm::Type;
using llvm::VACopyInst;
using llvm::Value;

constexpr unsigned CACHE_LINE_NUM = 32;
constexpr unsigned CACHE_LINE_SIZE = 16;
constexpr unsigned ARCH_SIZE = 8;

struct Var {
  Value *Val = nullptr;
  unsigned AddrB = 0;
  unsigned AddrE = 0;
  unsigned LineB = 0;
  unsigned LineE = 0;
  unsigned AgeSize = 0;
  unsigned AgeIndex = 0;
  Type *ty = nullptr;
  unsigned alignment = 1;
};

enum class AccessKind {
  Load,
  Store,
  Call,
  Intrinsic,
  Unknown,
};

struct AbstractMemoryObject {
  const Value *Base = nullptr;
  std::string Name;
  uint64_t Size = 0;
  unsigned Alignment = 1;
  bool IsHeap = false;
  bool IsGlobal = false;
  bool IsStack = false;
  bool IsArgument = false;
  bool IsPrecise = true;
};

struct ResolvedAccess {
  const Instruction *Inst = nullptr;
  const Value *Base = nullptr;
  uint64_t OffsetBegin = 0;
  uint64_t OffsetEnd = 0;
  AccessKind Kind = AccessKind::Unknown;
  bool IsRead = false;
  bool IsWrite = false;
  bool IsPrecise = false;
};

struct SpectreObservation {
  const Instruction *Inst = nullptr;
  const Value *MemoryObject = nullptr;
  std::string ObjectName;
  llvm::SmallVector<unsigned, 4> CacheLines;
  bool DivergesFromArchitectural = false;
  bool FromCall = false;
};

struct SpectreFinding {
  const BranchInst *Branch = nullptr;
  const BasicBlock *MergeBlock = nullptr;
  llvm::SmallVector<const BasicBlock *, 8> ExploredThenBlocks;
  llvm::SmallVector<const BasicBlock *, 8> ExploredElseBlocks;
  llvm::SmallVector<SpectreObservation, 4> ThenObservations;
  llvm::SmallVector<SpectreObservation, 4> ElseObservations;
  bool HasDivergence = false;
};

struct SpectreAnalysisResult {
  llvm::SmallVector<SpectreFinding, 4> Findings;
  unsigned ArchitecturalMisses = 0;
  unsigned ArchitecturalHits = 0;
  unsigned SpeculativeMisses = 0;
  unsigned SpeculativeHits = 0;

  bool hasFindings() const { return !Findings.empty(); }
  void dump(llvm::raw_ostream &os) const;
};

class CacheModel {
public:
  std::set<unsigned> cacheRecord;
  unsigned CacheLineNum;
  unsigned CacheLineSize;
  unsigned CacheSetNum;
  unsigned CacheLinesPerSet;

  unsigned MaxAddr;
  std::vector<unsigned> Ages;
  bool MustMod;
  llvm::ValueMap<Value *, Var *> Vars;

  unsigned HitCount;
  unsigned MissCount;
  unsigned SpecuHitCount;
  unsigned SpecuMissCount;

  static unsigned GetTySize(Type *ty);
  static int GEPInstPos(GetElementPtrInst &I, unsigned &from, unsigned &to);

  CacheModel(unsigned lineSize, unsigned lineNum, unsigned setNum,
             bool must = true);
  ~CacheModel();

  void SetMaxAddr(unsigned addr) { MaxAddr = addr; }
  void SetAges(const std::vector<unsigned> &ages);
  void SetVarsMap(const llvm::ValueMap<Value *, Var *> &vars);

  bool ConfigConsistent(const CacheModel *model) const;
  bool CacheConsistent(const CacheModel *model) const;
  bool isVarPartiallyCached(const Var *var) const;

  unsigned GetAge(Value *var, unsigned offset = 0) const;
  unsigned SetAge(Value *var, unsigned age, unsigned offset = 0);
  unsigned SetAge(Value *var, unsigned age, unsigned b, unsigned e);

  unsigned Access(Value *var, unsigned offset = 0);
  unsigned Access(Value *var, bool force);
  unsigned LocateVar(Value *var, unsigned offset) const;
  unsigned AddVar(Value *var, Type *ty, unsigned alignment = 1);

  CacheModel *fork() const;
  bool equal(const CacheModel *model) const;
  CacheModel *merge(CacheModel *mod);
  bool widenFrom(const CacheModel &previous);
  void invalidateAll();

  void dump(bool verbose = false) const;
  bool isInCache(const std::string &varName) const;
};

class CacheSpecuAnalysis : public llvm::InstVisitor<CacheSpecuAnalysis> {
public:
  CacheSpecuAnalysis(Function &F, DominatorTree &DT, PostDominatorTree &PDT,
                     AliasAnalysis *AA, unsigned lineSize = CACHE_LINE_SIZE,
                     unsigned lineNum = CACHE_LINE_NUM, unsigned setNum = 1,
                     unsigned depth = 4, unsigned merge = 0);

  bool SpecuSim(BasicBlock *from, BasicBlock *to, CacheModel *init = nullptr);

  const SpectreAnalysisResult &getResult() const { return Result; }
  const llvm::SmallVectorImpl<SpectreFinding> &getFindings() const {
    return Result.Findings;
  }
  bool hasFindings() const { return Result.hasFindings(); }

  void dump(int mod);
  bool wideningOp(CacheModel *last, CacheModel *current);

  bool IsValueInCache(Instruction *inst);
  bool GetInstCacheRange(Value *inst, GlobalVariable *&GV, unsigned &offset_b,
                         unsigned &offset_e);
  std::vector<Value *> GetAlias(Value *val, unsigned offset = 0);
  void InitModel();
  void InitModel(GlobalVariable *var, unsigned b, unsigned e);
  void ExtractGEPC(ConstantExpr *source, Value *&target, unsigned &offset);

  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitBitCastInst(llvm::BitCastInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitCallInst(CallInst &I);
  void visitPHINode(PHINode &I);
  void visitSelectInst(SelectInst &I);
  void visitIntrinsicInst(IntrinsicInst &I);
  void visitVACopyInst(VACopyInst &I);
  void visitBranchInst(BranchInst &I);
  void visitGetElementPtrInst(GetElementPtrInst &I);
  void visitInstruction(Instruction &I);

private:
  Function *F;
  DominatorTree *DT;
  PostDominatorTree *PDT;
  AliasAnalysis *AA;
  CacheModel *model;

  unsigned SpeculationDepth;
  unsigned MergeOption;
  bool RunSpeculation;
  bool cacheChanged;

  SpectreAnalysisResult Result;
  std::map<const BasicBlock *, CacheModel *> ArchitecturalInStates;
  std::map<const BasicBlock *, CacheModel *> ArchitecturalOutStates;
};

} // namespace spectre
