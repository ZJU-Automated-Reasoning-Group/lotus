//
// Created by prophe cheng on 2025/4/1.
//

#ifndef INDIRECTCALLANALYZER_MLTAPASS_H
#define INDIRECTCALLANALYZER_MLTAPASS_H

#include "llvm/IR/Operator.h"

#include "Alias/FPA/FLTAPass.h"

typedef pair<Type *, int> typeidx_t;
pair<Type *, int> typeidx_c(Type *Ty, int Idx);

typedef pair<size_t, int> hashidx_t;
pair<size_t, int> hashidx_c(size_t Hash, int Idx);

class MLTAPass : public FLTAPass {
public:
  ////////////////////////////////////////////////////////////////
  // Important data structures for type confinement, propagation,
  // and escapes.
  ////////////////////////////////////////////////////////////////
  DenseMap<size_t, map<int, FuncSet>> typeIdxFuncsMap;
  map<size_t, map<int, set<hashidx_t>>> typeIdxPropMap;
  set<size_t> typeEscapeSet;
  // Cap type: We cannot know where the type can be futher
  // propagated to. Do not include idx in the hash
  set<size_t> typeCapSet;
  // Functions that are actually stored to variables
  FuncSet StoredFuncs;
  // Alias struct pointer of a general pointer
  map<Function *, map<Value *, Value *>> AliasStructPtrMap;

  MLTAPass(GlobalContext *Ctx_) : FLTAPass(Ctx_) {
    ID = "multi layer type analysis";
    Ctx->util.LoadElementsStructNameMap(Ctx->Modules);
  }

  void analyzeIndCall(CallInst *callInst, FuncSet *FS) override;

  bool doInitialization(Module *) override;

  ////////////////////////////////////////////////////////////////
  // Target-related basic functions
  ////////////////////////////////////////////////////////////////
  void confineTargetFunction(Value *V, Function *F);
  bool typeConfineInInitializer(GlobalVariable *GV);
  bool typeConfineInFunction(Function *F);
  virtual void typeConfineInStore(StoreInst *SI);
  virtual void escapeFuncPointer(Value *PO, Instruction *I);
  bool typePropInFunction(Function *F);

  void collectAliasStructPtr(Function *F);

  ////////////////////////////////////////////////////////////////
  // API functions
  ////////////////////////////////////////////////////////////////
  // Use type-based analysis to find targets of indirect calls
  bool getTargetsWithLayerType(size_t TyHash, int Idx, FuncSet &FS);

  ////////////////////////////////////////////////////////////////
  // Util functions
  ////////////////////////////////////////////////////////////////
  bool isCompositeType(Type *Ty);
  Type *getFuncPtrType(Value *V);
  Value *recoverBaseType(Value *V);
  Function *getBaseFunction(Value *V);

  // type utils
  void escapeType(Value *V);
  void propagateType(Value *ToV, Type *FromTy, int Idx = -1);

  Type *getBaseType(Value *V, set<Value *> &Visited);
  Type *_getPhiBaseType(PHINode *PN, set<Value *> &Visited);
  bool nextLayerBaseType(Value *V, list<typeidx_t> &TyList, Value *&NextV,
                         set<Value *> &Visited);
  bool getGEPLayerTypes(GEPOperator *GEP, list<typeidx_t> &TyList);
  bool getBaseTypeChain(list<typeidx_t> &Chain, Value *V, bool &Complete);
  bool getDependentTypes(Type *Ty, int Idx, set<hashidx_t> &PropSet);
};

#endif // INDIRECTCALLANALYZER_MLTAPASS_H
