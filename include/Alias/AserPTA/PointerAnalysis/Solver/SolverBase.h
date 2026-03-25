// the basic framework for andersen-based algorithm, including common routines
// override neccessary ones, and the call will be STATICALLY redirected to it
#ifndef ASER_PTA_SOLVERBASE_H
#define ASER_PTA_SOLVERBASE_H

#define DEBUG_TYPE "pta"
#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/CallGraph.h"
#include "Alias/AserPTA/PointerAnalysis/Models/MemoryModel/MemModelTrait.h"
#include "Alias/AserPTA/PointerAnalysis/Solver/PointsTo/PointsToSelector.h"
#include "Alias/AserPTA/Util/Log.h"
#include "Alias/AserPTA/Util/Statistics.h"

extern llvm::cl::opt<bool> ConfigPrintConstraintGraph;
extern llvm::cl::opt<bool> ConfigPrintCallGraph;
extern llvm::cl::opt<bool> ConfigDumpPointsToSet;
extern llvm::cl::opt<bool> ConfigUseOnTheFlyCallGraph;

namespace aser {

template <typename ctx>
class CallGraph;

template <typename LangModel, typename SubClass>
class SolverBase {
private:
    struct Noop {
        template <typename... Args>
        __attribute__((always_inline)) void operator()(Args &&...) {}
    };
    std::unique_ptr<LangModel> langModel;

    LOCAL_STATISTIC(ProcessedCopy, "Number of Processed Copy Edges");
    LOCAL_STATISTIC(ProcessedLoad, "Number of Processed Load Edges");
    LOCAL_STATISTIC(ProcessedStore, "Number of Processed Store Edges");
    LOCAL_STATISTIC(ProcessedOffset, "Number of Processed Offset Edges");

    LOCAL_STATISTIC(EffectiveCopy, "Number of Effective Copy Edges");
    LOCAL_STATISTIC(EffectiveLoad, "Number of Effective Load Edges");
    LOCAL_STATISTIC(EffectiveStore, "Number of Effective Store Edges");
    LOCAL_STATISTIC(EffectiveOffset, "Number of Effective Offset Edges");

public:
    using LMT = LangModelTrait<LangModel>;
    using MemModel = typename LMT::MemModelTy;
    using MMT = MemModelTrait<MemModel>;
    using ctx = typename LangModelTrait<LangModel>::CtxTy;
    using CT = CtxTrait<ctx>;
    using ObjTy = typename MMT::ObjectTy;

protected:
    using PtsTy = typename LMT::PointsToTy;
    using PT = PTSTrait<PtsTy>;

    using CallGraphTy = CallGraph<ctx>;
    using CallNodeTy = typename CallGraphTy::NodeType;
    using ConsGraphTy = ConstraintGraph<ctx>;
    using CGNodeTy = CGNodeBase<ctx>;
    using PtrNodeTy = CGPtrNode<ctx>;
    using ObjNodeTy = CGObjNode<MemModel>;

    ConsGraphTy *consGraph;
    llvm::SparseBitVector<> updatedFunPtrs;

    // TODO: the intersection on pts should be done through PtsTrait for better extensibility
    llvm::DenseMap<PtrNodeTy *, llvm::DenseSet<NodeID>> handledGEPMap;

    inline void updateFunPtr(NodeID indirectNode) {
        updatedFunPtrs.set(indirectNode);
    }

    inline bool resolveFunPtrs() {
        bool reanalyze = LMT::updateFunPtrs(langModel.get(), updatedFunPtrs);
        updatedFunPtrs.clear();

        return reanalyze;
    }

    // Hook for subclasses to populate call graph with pre-built results (e.g., from DyckAA, FPA)
    // This is called AFTER buildInitModel() but BEFORE constructConsGraph(), so the call graph
    // can be populated before constraint graph construction begins.
    //
    // IMPORTANT: The call graph affects constraint graph construction because:
    // 1. buildInitCallGraph() (called during constructConsGraph()) traverses the call graph
    // 2. For each call edge, it calls processCallSite() which adds constraints
    // 3. Indirect calls need to be resolved BEFORE this traversal happens
    //
    // To use this hook:
    // 1. Override this method in your solver subclass
    // 2. Use getLangModel() to access the language model
    // 3. Query your pre-built call graph (e.g., DyckAA, FPA) for indirect call resolutions
    // 4. For each resolved indirect call, you need to call the module's resolveCallTo() method
    //    However, this requires callbacks (beforeNewNode, onNewDirect, onNewInDirect, onNewEdge)
    //    which are defined in ConsGraphBuilder. You may need to:
    //    - Access the language model's internal ConsGraphBuilder instance, OR
    //    - Manually populate the call graph structure before constructConsGraph() runs
    //
    // Returns true if any indirect calls were resolved (currently unused, but may be useful
    // for future optimizations)
    virtual bool populatePreBuiltCallGraph() {
        // Default implementation: no pre-built call graph
        // auto *lm = this->getLangModel(); // or use LMT traits
        // 1) query your pre-built CG (e.g., DyckAA/FPA) for indirect call resolutions
        // 2) for each resolved (callsite -> target), call
        //    lm->getCtxModule()->resolveCallTo(...) via ConsGraphBuilder callbacks,
        //    or whatever accessor you expose to the module
        return false;
    }

    // seems like the scc becomes the bottleneck, need to merge large scc
    // return the super node of the scc
    CGNodeTy *processCopySCC(const std::vector<CGNodeTy *> &scc) {
        assert(scc.size() > 1);

        CGNodeTy *superNode = scc.front();
        for (auto nit = ++(scc.begin()), nie = scc.end(); nit != nie; nit++) {
            // merge pts in scc all into front
            this->processCopy(*nit, superNode);
        }

        // collapse scc to the front node
        this->getConsGraph()->collapseSCCTo(scc, superNode);

        // if there is a function ptr in the scc, update the function ptr
        if (superNode->isFunctionPtr()) {
            this->updateFunPtr(superNode->getNodeID());
        }

        for (auto cit = superNode->succ_copy_begin(), cie = superNode->succ_copy_end(); cit != cie; cit++) {
            this->processCopy(superNode, *cit);
        }

        return superNode;
    }

    // some helper function that might be needed by subclasses
    constexpr inline bool processAddrOf(CGNodeTy *src, CGNodeTy *dst) const;
    inline bool processCopy(CGNodeTy *src, CGNodeTy *dst);

    // TODO: only process diff pts
    template <typename CallBack=Noop>
    inline bool processOffset(CGNodeTy *src, CGNodeTy *dst, CallBack callBack=Noop{}) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        ProcessedOffset ++;

        // TODO: use llvm::cast in debugging build
        // gep for sure create a pointer node
        CGPtrNode<ctx> *ptrNode = static_cast<CGPtrNode<ctx> *>(dst);
        // assert(ptrNode);

        // we must be handling a getelemntptr instruction if we are indexing a object
        const auto *gep = static_cast<const llvm::GetElementPtrInst *>(ptrNode->getPointer()->getValue());
        // assert(gep);

        // TODO: the intersection on pts should be done through PtsTrait for better extensibility
        auto &handled = handledGEPMap.try_emplace(ptrNode).first->second;
        const auto &curPts = PT::getPointsTo(src->getNodeID());

        bool changed = false;
        std::vector<ObjNodeTy *> nodeVec;
        std::vector<NodeID> newIds;
        for (auto it = curPts.begin(), ie = curPts.end(); it != ie; ++it) {
            NodeID id = *it;
            if (!handled.count(id))
                newIds.push_back(id);
        }

        if (newIds.empty())
            return false;

        for (NodeID id : newIds)
            handled.insert(id); // update handled gep

        nodeVec.reserve(newIds.size());
        // We need to cache all the nodes here because the PT might be modified and the iterator might be invalid
        for (NodeID id : newIds) {
            // TODO: use llvm::cast in debugging build
            auto objNode = static_cast<ObjNodeTy *>((*consGraph)[id]);
            nodeVec.push_back(objNode);
        }

        // update the cached pts
        for (auto objNode : nodeVec) {
            // this might create new object, thus modify the points-to set
            CGNodeTy *fieldObj = LMT::indexObject(this->getLangModel(), objNode, gep);
            if (fieldObj == nullptr) {
                continue;
            }

            if (!PT::has(ptrNode->getNodeID(), fieldObj->getNodeID())) {
                // insert an addr_of constraint if ptrNode does not points to field object previous
//#ifndef NO_ADDR_OF_FOR_OFFSET
              // insert an addr_of constraint if ptrNode does not points to field object previous
                // this is the major source for newly inserted constraints
                // remove this but relying on solver to handle it correctly
                // can improve both performance and memory effciency
                // but the visualization of the constraint graph will be affected.
                this->consGraph->addConstraints(fieldObj, ptrNode, Constraints::addr_of);
//#endif
                //this->consGraph->addConstraints(fieldObj, ptrNode, Constraints::addr_of);
                callBack(fieldObj, ptrNode);
                changed = true;
            }
        }

        if (changed) {
            EffectiveOffset ++;
        }
        return changed;
    }

    // TODO: only process diff pts
    // src --LOAD-->dst
    // for every node in pts(src):
    //     node --COPY--> dst
    template <typename CallBack = Noop>
    bool processLoad(CGNodeTy *src, CGNodeTy *dst,
                     CallBack callBack = Noop{}, const typename PT::PtsTy *diffPts = nullptr) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        ProcessedLoad ++;
        if (diffPts == nullptr) {
            diffPts = &PT::getPointsTo(src->getNodeID());
        }

        bool changed = false;
        for (auto it = diffPts->begin(), ie = diffPts->end(); it != ie; it++) {
            auto node = (*consGraph)[*it];
            node = node->getSuperNode();
            if (consGraph->addConstraints(node, dst, Constraints::copy)) {
                changed = true;
                callBack(node, dst);
            }
        }

        if (changed) {
            EffectiveLoad ++;
        }
        return changed;
    }

    // TODO: only process diff pts
    // src --STORE-->dst
    // for every node in pts(dst):
    //      src --COPY--> node
    template <typename CallBack = Noop>
    bool processStore(CGNodeTy *src, CGNodeTy *dst,
                      CallBack callBack = Noop{}, const typename PT::PtsTy *diffPts = nullptr) {
        assert(!src->hasSuperNode() && !dst->hasSuperNode());
        if (diffPts == nullptr) {
            diffPts = &PT::getPointsTo(dst->getNodeID());
        }

        ProcessedStore ++;
        bool changed = false;
        for (auto it = diffPts->begin(), ie = diffPts->end(); it != ie; it++) {
            auto node = (*consGraph)[*it];
            node = node->getSuperNode();

            if (consGraph->addConstraints(src, node, Constraints::copy)) {
                changed = true;
                callBack(src, node);
            }
        }

        if (changed) {
            EffectiveStore ++;
        }
        return changed;
    }

    void solve() {
        // this is the main entrance of the pointer analysis, which performs the pointer analysis with on-the-fly call graph construction
        if (ConfigUseOnTheFlyCallGraph) {
            bool reanalyze;
            do {
                static_cast<SubClass *>(this)->runSolver(*langModel);
                // resolve indirect calls using the points-to information.
                reanalyze = resolveFunPtrs();
            } while (reanalyze);
        } else {
            // Run solver once without on-the-fly callgraph construction
            // NOTE: To use a pre-built call graph (e.g., DyckAA, FPA), override
            // populatePreBuiltCallGraph() in your solver subclass. This hook is called
            // in analyze() BEFORE constructConsGraph(), allowing the call graph to be
            // populated before constraint graph construction begins.
            static_cast<SubClass *>(this)->runSolver(*langModel);
        }
        // llvm::outs() << this->getConsGraph()->getNodeNum();
    }

    [[nodiscard]]
    inline LangModel *getLangModel() const {
        return this->langModel.get();
    }

    void dumpPointsTo() {
        std::error_code ErrInfo;
        std::string fileName;
        llvm::raw_string_ostream os(fileName);
        os << "PTS" << this;

        llvm::ToolOutputFile F(os.str(), ErrInfo, llvm::sys::fs::OF_None);
        if (!ErrInfo) {
            // dump the points to set

            // 1st, dump the Object Node Information
            for (auto it = this->getConsGraph()->begin(), ie = this->getConsGraph()->end();
                 it != ie; it++) {
                CGNodeTy *node = *it;
                if (llvm::isa<ObjNodeTy>(node)) {
                    // dump the information
                    F.os() << "Object " << node->getNodeID() << " : \n";
                    F.os() << node->toString() << "\n";
                }
            }

            // 2nd, dump the points to set of every node
            for (auto it = this->getConsGraph()->begin(), ie = this->getConsGraph()->end();
                 it != ie; it++) {
                CGNodeTy *node = *it;
                F.os() << node->toString() << " : ";
                F.os() << "{";
                bool isFirst = true;

                for (auto it = PT::begin(node->getNodeID()), ie = PT::end(node->getNodeID());
                     it != ie; it++) {
                    if (isFirst) {
                        F.os() << *it;
                        isFirst = false;
                    } else {
                        F.os() << " ," << *it;
                    }

                }
                F.os() << "}\n\n\n";
            }

            if (!F.os().has_error()) {
                llvm::outs() << "\n";
                F.keep();
                return;
            }
        }
    }

public:
    virtual ~SolverBase() {
        CT::release();
        PT::clearAll();
    }

    /// Expose language model for clients outside the solver (e.g., SVFGBuilder).
    /// This is read-only access; mutating the model from outside is unsupported.
    [[nodiscard]] inline LangModel *getLangModelForClients() const {
        return langModel.get();
    }

    // analyze the give module with specified entry function
    bool analyze(llvm::Module *module, llvm::StringRef entry = "main") {
        assert(langModel == nullptr && "can not run pointer analysis twice");
        // ensure the points to set are cleaned.
        // TODO: support different point-to set instance for different PTA instance
        // new they all share a global vector to store it.
        PT::clearAll();

        // using language model to construct language model
        langModel.reset(LMT::buildInitModel(module, entry));
        // If on-the-fly call graph construction is disabled, let subclasses
        // populate the call graph ahead of constraint graph construction.
        if (!ConfigUseOnTheFlyCallGraph) {
            bool populated = static_cast<SubClass *>(this)->populatePreBuiltCallGraph();
            if (!populated) {
                LOG_WARN("on-the-fly call graph disabled, but populatePreBuiltCallGraph() was not overridden; "
                         "indirect calls may remain unresolved");
            }
        }

        LMT::constructConsGraph(langModel.get());

        consGraph = LMT::getConsGraph(langModel.get());

        std::string fileName;
        llvm::raw_string_ostream os(fileName);
        os << this;

        if (ConfigPrintConstraintGraph) {
            WriteGraphToFile("ConstraintGraph_Initial_" + os.str(), *this->getConsGraph());
        }

        // subclass might override solve() directly for more aggressive overriding
        static_cast<SubClass *>(this)->solve();

        LOG_DEBUG("PTA constraint graph node number {}, "
                  "callgraph node number {}",
                  this->getConsGraph()->getNodeNum(),
                  this->getCallGraph()->getNodeNum());

        if (ConfigPrintConstraintGraph) {
            WriteGraphToFile("ConstraintGraph_Final_" + os.str(), *this->getConsGraph());
        }
        if (ConfigPrintCallGraph) {
            WriteGraphToFile("CallGraph_Final_" + os.str(), *this->getCallGraph());
        }
        if (ConfigDumpPointsToSet) {
            // dump the points to set of every pointers
            dumpPointsTo();
        }

        return false;
    }

    CGNodeTy *getCGNode(const ctx *context, const llvm::Value *V) const {
        NodeID id = LMT::getSuperNodeIDForValue(langModel.get(), context, V);
        return (*consGraph)[id];
    }

    void getPointsTo(const ctx *context, const llvm::Value *V, std::vector<const ObjTy *> &result) const {
        assert(V->getType()->isPointerTy());

        // get the node value
        NodeID node = LMT::getSuperNodeIDForValue(langModel.get(), context, V);
        if (node == INVALID_NODE_ID) {
            return;
        }

        for (auto it = PT::begin(node), ie = PT::end(node); it != ie; it++) {
            auto objNode = llvm::dyn_cast<ObjNodeTy>(consGraph->getNode(*it));
            assert(objNode);
            if (objNode->isSpecialNode()) {
                continue;
            }
            result.push_back(objNode->getObject());
        }
    }

    const llvm::Type *getPointedType(const ctx *context, const llvm::Value *V) const {
        std::vector<const ObjTy *> result;
        getPointsTo(context, V, result);

        if (result.size() == 1) {
            const llvm::Type *type = result[0]->getType();
            // the allocation site is a pointer type
            assert(type->isPointerTy());
            // get the actually allocated object type
            return type->getPointerElementType();
        }
        // do not know the type
        return nullptr;
    }

    [[nodiscard]] bool alias(const ctx *c1, const llvm::Value *v1, const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::intersectWithNoSpecialNode(n1, n2);
    }

    [[nodiscard]] bool aliasIfExsit(const ctx *c1, const llvm::Value *v1, const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        if(n1 == INVALID_NODE_ID || n2 == INVALID_NODE_ID) {
            return false;
        }
        return PT::intersectWithNoSpecialNode(n1, n2);
    }

    [[nodiscard]] bool hasIdenticalPTS(const ctx *c1, const llvm::Value *v1,
                                       const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::equal(n1, n2);
    }

    [[nodiscard]] bool containsPTS(const ctx *c1, const llvm::Value *v1,
                                   const ctx *c2, const llvm::Value *v2) const {
        assert(v1->getType()->isPointerTy() && v2->getType()->isPointerTy());

        NodeID n1 = LMT::getSuperNodeIDForValue(langModel.get(), c1, v1);
        NodeID n2 = LMT::getSuperNodeIDForValue(langModel.get(), c2, v2);

        assert(n1 != INVALID_NODE_ID && n2 != INVALID_NODE_ID && "can not find node in constraint graph!");
        return PT::contains(n1, n2);
    }



    // Delegator of the language model
    [[nodiscard]]
    inline ConsGraphTy *getConsGraph() const {
        return LMT::getConsGraph(langModel.get());
    }

    [[nodiscard]]
    inline const CallGraphTy *getCallGraph() const {
        return LMT::getCallGraph(langModel.get());
    }

    [[nodiscard]]
    inline llvm::StringRef getEntryName() const {
        return LMT::getEntryName(this->getLangModel());
    }

    [[nodiscard]]
    inline const llvm::Module *getLLVMModule() const {
        return LMT::getLLVMModule(this->getLangModel());
    }

    [[nodiscard]]
    inline const CallGraphNode<ctx> *getDirectNode(const ctx *C, const llvm::Function *F) {
        return LMT::getDirectNode(this->getLangModel(), C, F); //->getDirectNode(C, F);
    }

    [[nodiscard]]
    inline const CallGraphNode<ctx> *getDirectNodeOrNull(const ctx *C, const llvm::Function *F) {
        return LMT::getDirectNodeOrNull(this->getLangModel(), C, F);
    }

    [[nodiscard]]
    inline const InDirectCallSite<ctx> *getInDirectCallSite(const ctx *C, const llvm::Instruction *I) {
        return LMT::getInDirectCallSite(this->getLangModel(), C, I);
    }
};

template <typename LangModel, typename SubClass>
constexpr bool SolverBase<LangModel, SubClass>::processAddrOf(CGNodeTy *src, CGNodeTy *dst) const {
#ifndef NDEBUG
    // should already been handled
    assert(!PT::insert(dst->getNodeID(), src->getNodeID()));
#endif
    return false;
}

// site. pts(dst) |= pts(src);
template <typename LangModel, typename SubClass>
bool SolverBase<LangModel, SubClass>::processCopy(CGNodeTy *src, CGNodeTy *dst) {
    ProcessedCopy++;
    if (PT::unionWith(dst->getNodeID(), src->getNodeID())) {
        if (dst->isFunctionPtr()) {
            // node used for indirect call
            this->updateFunPtr(dst->getNodeID());
        }
        EffectiveCopy++;
        return true;
    }
    return false;
}

}  // namespace aser

#undef DEBUG_TYPE

#endif
