//
// Created by peiming on 10/22/19.
//

// memory langModel build the connection between memory block and static object

// for field-insensitive memory langModel,
// one memory block -> one static object

// for field-sensitive memory langModel
// one memory block -> multiple static objects

#ifndef ASER_PTA_FIMEMMODEL_H
#define ASER_PTA_FIMEMMODEL_H

#include <llvm/IR/LegacyPassManager.h>

#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FICanonicalizer.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/FieldInsensitive/FIObject.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/MemModelTrait.h"
#include "Alias/AserPTA/PointerAnalysis/Program/Pointer.h"
#include "Alias/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/AserPTA/Util/SingleInstanceOwner.h"

namespace aser {

template <typename ctx>
class FIMemModel : public SingleInstanceOwner<FIObject<ctx>> {
    using CT = CtxTrait<ctx>;
    using Self = FIMemModel<ctx>;
    using ObjNode = CGObjNode<Self>;
    using PtrNode = CGPtrNode<ctx>;
    using BaseNode = CGNodeBase<ctx>;
    using ConsGraphTy = ConstraintGraph<ctx>;
    using PtrOwner = SingleInstanceOwner<Pointer<ctx>>;
    using Canonicalizer = FICanonicalizer;

    const PtrOwner &ptrOwner;  // the manager for pointer
    ConsGraphTy &consGraph;
    ObjNode *nullObjNode = nullptr;
    ObjNode *uniObjNode = nullptr;

    template <typename PT>
    inline ObjNode *createNode(const ctx *C, const llvm::Value *V, const AllocType type) {
        const FIObject<ctx> *obj = this->create(C, V, type);
        auto ret = consGraph.template addCGNode<ObjNode, PT>(obj);
        const_cast<FIObject<ctx> *>(obj)->setObjNode(ret);
        return ret;
    }

public:
    FIMemModel(ConsGraphTy &consGraph, const PtrOwner &owner) : consGraph(consGraph), ptrOwner(owner) {}

    template <typename PT>
    inline ObjNode *allocNullObj(const llvm::Module *module) {
        assert(!nullObjNode);
        auto v = llvm::ConstantPointerNull::get(llvm::Type::getInt8PtrTy(module->getContext()));
        return this->template createNode<PT>(CT::getGlobalCtx(), v, AllocType::Null);
    }

    template <typename PT>
    inline ObjNode *allocUniObj(const llvm::Module *module) {
        assert(!uniObjNode);
        auto v = llvm::UndefValue::get(llvm::Type::getInt8PtrTy(module->getContext()));
        return this->template createNode<PT>(CT::getGlobalCtx(), v, AllocType::Universal);
    }

    template <typename PT>
    inline void handleMemCpy(const ctx *C, const llvm::MemCpyInst *memCpy, PtrNode *src, PtrNode *dst) {
        // a temporal node
        ObjNode *memCpyNode = this->template createNode<PT>(C, memCpy, AllocType::Anonymous);
        // a MemCpy => <src>--load--><tmp>--store--><dst>;
        consGraph.addConstraints(src, memCpyNode, Constraints::load);
        consGraph.addConstraints(memCpyNode, dst, Constraints::store);
    }

    template <typename PT>
    inline ObjNode *allocStackObj(const ctx *c, const llvm::AllocaInst *I) {
        return this->template createNode<PT>(c, I, AllocType::Stack);
    }

    template <typename PT>
    inline ObjNode *allocFunction(const llvm::Function *f) {
        return this->template createNode<PT>(CT::getGlobalCtx(), f, AllocType::Functions);
    }

    template <typename PT>
    inline ObjNode *allocGlobalVariable(const llvm::GlobalVariable *g) {
        return this->template createNode<PT>(CT::getGlobalCtx(), g, AllocType::Globals);
    }

    template <typename PT>
    inline ObjNode *allocHeapObj(const ctx *c, const llvm::Instruction *I) {
        return this->template createNode<PT>(c, I, AllocType::Heap);
    }

    template <typename PT>
    inline void processScalarGlobals(const llvm::GlobalVariable *gVar, const llvm::Constant *C) {
        if (llvm::isa<llvm::PointerType>(C->getType())) {
            // strip the offset+alias
            const auto *value = Canonicalizer::canonicalize(C);
            auto obj = this->get(CT::getGlobalCtx(), value, AllocType::Globals);
            auto ptr = this->get(CT::getGlobalCtx(), gVar, AllocType::Globals);

            BaseNode *objNode = obj->getObjNode();
            BaseNode *ptrNode = ptr->getObjNode();
            // obj --addr_of--> global
            consGraph.addConstraints(objNode, ptrNode, Constraints::addr_of);
        }
    }

    template <typename PT>
    inline void processAggregateGlobals(const llvm::GlobalVariable *gVar, const llvm::Constant *c) {
        assert(llvm::isa<llvm::ConstantArray>(c) || llvm::isa<llvm::ConstantDataSequential>(c) ||
               llvm::isa<llvm::ConstantStruct>(c));
        // field-insensitive
        for (unsigned i = 0, e = c->getNumOperands(); i != e; ++i) {
            // every field in the object are copied to the starting address of
            // their parents.
            processInitializer<PT>(gVar, llvm::cast<llvm::Constant>(c->getOperand(i)));
        }
    }

    template <typename PT>
    void processInitializer(const llvm::GlobalVariable *gVar, const llvm::Constant *initializer) {
        if (initializer->isNullValue()) {
            // skip zero initializer ? does it matter?
            // if so, simply link it to null obj
        } else if (initializer->getType()->isSingleValueType()) {
            processScalarGlobals<PT>(gVar, initializer);
        } else {
            processAggregateGlobals<PT>(gVar, initializer);
        }
    }

    template <typename PT>
    void initializeGlobal(const llvm::GlobalVariable *gVar, const llvm::DataLayout &DL) {
        if (gVar->hasInitializer()) {
            processInitializer<PT>(gVar, gVar->getInitializer());
        } else {
            // an extern symbol, conservatively can point to anything
            // link it to universal object node
            // this->consGraph->addConstraints(nodeManager.getUniObjNode(),
            // gNode, Constraints::addr_of);
        }
    }
};

template <typename ctx>
struct MemModelTrait<FIMemModel<ctx>> : public SingleInstanceOwner<FIObject<ctx>> {
    using CtxTy = ctx;
    using ObjectTy = FIObject<ctx>;
    using Canonicalizer = FICanonicalizer;

    // whether GEP will be collapse
    static const bool COLLAPSE_GEP = true;
    // whether BitCast will be collapse
    static const bool COLLAPSE_BITCAST = true;
    // whether type information is necessary
    // for field-sensitive pointer analysis, we do not need type information
    static const bool NEED_TYPE_INFO = false;

    // set analysis usage
    static void getAnalysisUsage(llvm::AnalysisUsage &AU) { AU.addRequired<CanonicalizeGEPPass>(); }

    // add required passes into pass manager
    static inline void addDependentPasses(llvm::legacy::PassManager &passes) { passes.add(new CanonicalizeGEPPass()); }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateNullObj(FIMemModel<ctx> &model, const llvm::Module *module) {
        return model.template allocNullObj<PT>(module);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateUniObj(FIMemModel<ctx> &model, const llvm::Module *module) {
        return model.template allocUniObj<PT>(module);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateFunction(FIMemModel<ctx> &model, const llvm::Function *fun) {
        return model.template allocFunction<PT>(fun);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateGlobalVariable(FIMemModel<ctx> &model,
                                                                     const llvm::GlobalVariable *gVar,
                                                                     const llvm::DataLayout &) {
        return model.template allocGlobalVariable<PT>(gVar);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateStackObj(FIMemModel<ctx> &model, const ctx *context,
                                                               const llvm::AllocaInst *gVar, const llvm::DataLayout &) {
        return model.template allocStackObj<PT>(context, gVar);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *allocateHeapObj(FIMemModel<ctx> &model, const ctx *context,
                                                              const llvm::Instruction *callsite,
                                                              const llvm::DataLayout &, const llvm::Type *T) {
        return model.template allocHeapObj<PT>(context, callsite);
    }

    template <typename PT>
    inline static CGObjNode<FIMemModel<ctx>> *indexObject(FIMemModel<ctx> &model, const FIObject<ctx> *obj,
                                                          const llvm::GetElementPtrInst *gep) {
        llvm_unreachable("can not index object in field insensitive pointer analysis");
    }

    template <typename PT>
    inline static void handleMemCpy(FIMemModel<ctx> &model, const ctx *C, const llvm::MemCpyInst *memCpy,
                                    CGPtrNode<ctx> *src, CGPtrNode<ctx> *dst) {
        return model.template handleMemCpy<PT>(C, memCpy, src, dst);
    }

    template <typename PT>
    inline static void initializeGlobal(FIMemModel<ctx> &memModel, const llvm::GlobalVariable *gVar,
                                        const llvm::DataLayout &DL) {
        memModel.template initializeGlobal<PT>(gVar, DL);
    }
};

}  // namespace aser

#endif