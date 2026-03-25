//
// Created by Yanze on 6/27/20.
// Based on WavePropagation.h
//
#ifndef ASER_PTA_DEEPPROPAGATION_H
#define ASER_PTA_DEEPPROPAGATION_H

#include <map>

#include "Alias/AserPTA/PointerAnalysis/Solver/SolverBase.h"
#include "Alias/AserPTA/PointerAnalysis/Graph/ConstraintGraph/SCCIterator.h"

namespace aser {

template <typename LangModel>
class DeepPropagation : public SolverBase<LangModel, DeepPropagation<LangModel>> {
public:
    using super = SolverBase<LangModel, DeepPropagation<LangModel>>;
    using PtsTy = typename super::PtsTy;
    using PT = PTSTrait<PtsTy>;
    using ConsGraphTy = ConstraintGraph<typename super::ctx>;
    using CGNodeTy = CGNodeBase<typename super::ctx>;
    using GT = llvm::GraphTraits<ConsGraphTy>;
    using ctx = typename super::ctx;
    using LMT = typename super::LMT;

    using Edge = std::tuple<const CGNodeTy *, const CGNodeTy *, Constraints>;

    std::vector<typename PT::PtsTy> cachedPTS;
    std::map<Edge, typename PT::PtsTy> edgeCachedPTS;
    std::vector<CGNodeTy *> blackNodes;

protected:
    void runSolver(LangModel &langModel) {
        bool changed;
        ConsGraphTy &consGraph = *(super::getConsGraph());
        cachedPTS.clear();

        // enlarge the cachedPTS if necessary, after processing offset constraints, new node can be added
        while (cachedPTS.size() != consGraph.getNodeNum()) {
            cachedPTS.emplace_back();
        }

        std::stack<std::vector<CGNodeTy *>> copySCCStack;
        // Step 1: SCC detection
        auto copy_it = scc_begin<ctx, Constraints::copy, false>(consGraph);
        auto copy_ie = scc_end<ctx, Constraints::copy, false>(consGraph);

        for (; copy_it != copy_ie; ++copy_it) {
            copySCCStack.push(*copy_it);
        }

        // Step 2: Wave propagation
        while (!copySCCStack.empty()) {
            const std::vector<CGNodeTy *> &scc = copySCCStack.top();
            if (scc.size() > 1) {
                auto superNode = super::processCopySCC(scc);
                cachedPTS[superNode->getNodeID()] = PT::getPointsTo(superNode->getNodeID());
            } else {
                CGNodeTy *curNode = scc.front();
                typename PT::PtsTy diffPTS;
                diffPTS.intersectWithComplement(PT::getPointsTo(curNode->getNodeID()),
                                                cachedPTS[curNode->getNodeID()]);

                // only when the diffed pts is not empty
                if (!diffPTS.empty()) {
                    cachedPTS[curNode->getNodeID()] |= diffPTS;
                    for (auto cit = curNode->succ_copy_begin(), cie = curNode->succ_copy_end(); cit != cie; cit++) {
                        super::processCopy(curNode, *cit);
                    }
                }
            }
            copySCCStack.pop();
        }

        // Step 3: deep propagation
        do {
            size_t beforeResolve = this->getConsGraph()->getNodeNum();
            changed = false;
            size_t nodeNum = consGraph.getNodeNum();
            for (NodeID id = 0; id < nodeNum; id++) {
                CGNodeTy *curNode = consGraph.getNode(id);

                // for all load constraints
                // *it <--load-- curNode
                for (auto it = curNode->succ_load_begin(), ie = curNode->succ_load_end(); it != ie; it++) {
                    auto &cachedPts = edgeCachedPTS[std::make_tuple(curNode, *it, Constraints::load)];
                    // P_new_pts in the paper
                    typename PT::PtsTy newPTS;
                    // P_new_edges in the paper
                    typename PT::PtsTy newEdgePTS;
                    newEdgePTS.intersectWithComplement(PT::getPointsTo(curNode->getNodeID()),
                                                    cachedPts);
                    cachedPts |= newEdgePTS;
                    for (auto v : newEdgePTS) {
                        bool newCons = consGraph.addConstraints(consGraph.getNode(v), (*it), Constraints::copy);
                        if (newCons)
                            newPTS |= PT::getPointsTo(v);
                    }

                    typename PT::PtsTy diffPTS;
                    diffPTS.intersectWithComplement(newPTS, PT::getPointsTo((*it)->getNodeID()));
                    deepPropagate(*it, *it, diffPTS, changed);
                    unmarkBlack();
                }

                // for all store constraints
                // curNode <--store-- *it
                //    l    <--store--  r
                for (auto it = curNode->pred_store_begin(), ie = curNode->pred_store_end(); it != ie; it++) {
                    auto &cachedPts = edgeCachedPTS[std::make_tuple(*it, curNode, Constraints::store)];
                    typename PT::PtsTy newEdgePTS;
                    newEdgePTS.intersectWithComplement(PT::getPointsTo(curNode->getNodeID()),
                                                    cachedPts);
                    cachedPts |= newEdgePTS;

                    for (auto v : newEdgePTS) {
                        auto node = consGraph.getNode(v);
                        bool newCons = consGraph.addConstraints((*it), node, Constraints::copy);
                        if (newCons) {
                            // FIXME: will intersectWithComplement clear the pts first?
                            typename PT::PtsTy diff;
                            diff.intersectWithComplement(PT::getPointsTo((*it)->getNodeID()), PT::getPointsTo(v));
                            deepPropagate(node, (*it), diff, changed);
                        }
                    }
                    unmarkBlack();
                }

                // for all offset constraints
                 for (auto it = curNode->succ_offset_begin(), ie = curNode->succ_offset_end(); it != ie; it++) {
                     // offset already cached the diff points to set
                     changed |= super::processOffset(curNode, *it, [&](CGNodeTy *src, CGNodeTy *dst) {
                       auto addrNode = llvm::cast<typename super::ObjNodeTy>(src)->getAddrTakenNode();
                       super::processCopy(addrNode, dst);
                     });
                 }
            }
        } while (changed);

    }

    bool deepPropagate(CGNodeTy *start, CGNodeTy *stop, typename PT::PtsTy diff, bool &changed) {
        if (start->getColor() == Color::GREY) {
            return true;
        } else if (start->getColor() == Color::BLACK) {
            return false;
        }

        typename PT::PtsTy newPTS;
        // NOTE: getPointsTo returns const!!!
        typename PT::PtsTy& pts = const_cast<typename PT::PtsTy&>(PT::getPointsTo(start->getNodeID()));

        newPTS.intersectWithComplement(diff, pts);
        if (!newPTS.empty()) {
            // FIXME: I should use processCopy?
            // pts |= newPTS;
            if (bool updated = (pts |= newPTS)) {
                if (start->isFunctionPtr()) {
                    // node used for indirect call
                    super::updateFunPtr(start->getNodeID());
                    //LOG_INFO("function pointer updated");
                }
            }
            changed = true;
            // line 10 of algo 7
            for (auto cit = start->succ_copy_begin(), cie = start->succ_copy_end(); cit != cie; cit++) {
                if (*cit == stop || deepPropagate(*cit, stop, newPTS, changed)) {
                    start->setColor(Color::GREY);
                    return true;
                } else {
                    start->setColor(Color::BLACK);
                    blackNodes.push_back(start);
                    return false;
                }
            }
        } else {
            // line 19-21, this seems inefficient
            for (auto cit = start->succ_copy_begin(), cie = start->succ_copy_end(); cit != cie; cit++) {
                if (*cit == stop) {
                    start->setColor(Color::GREY);
                    return true;
                }
            }
        }
        start->setColor(Color::BLACK);
        blackNodes.push_back(start);
        return false;
    }

    void unmarkBlack() {
        for (CGNodeTy *n : blackNodes) {
            n->setColor(Color::DEFAULT);
        }
        blackNodes.clear();
    }

    friend super;
};

}  // namespace aser

#endif
