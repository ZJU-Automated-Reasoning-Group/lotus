//
// Created by peiming on 11/19/19.
//

#ifndef ASER_PTA_KORIGIN_H
#define ASER_PTA_KORIGIN_H

#include <Alias/AserPTA/Util/Log.h>

#include <llvm/ADT/StringSet.h>

#include "Alias/AserPTA/PointerAnalysis/Context/CtxTrait.h"
#include "Alias/AserPTA/PointerAnalysis/Context/KCallSite.h"

namespace aser {

template <uint32_t K, uint32_t L=1>
struct OriginsSetter;

// L is only useful in hybrid context,
// e.g., when use with <k-callsite + origin>, L=k+1 can make origin more precise

// TODO: support L > 1 to make it more accurate
// L is the length of the callchain that can be used to indentify an origin
template <uint32_t K, uint32_t L=1>
class KOrigin : public KCallSite<K * L> {
private:
    using self = KOrigin<K, L>;
    using super = KCallSite<K * L>;

    static std::function<bool(const self *, const llvm::Instruction *)> callback;

public:
    KOrigin() noexcept : super() {}
    KOrigin(const self *prevCtx, const llvm::Instruction *I) : super(prevCtx, I) {}

    static void setOriginRules(std::function<bool(const self *, const llvm::Instruction *)> cb) {
        callback = cb;
    }

    KOrigin(const self &) = delete;
    KOrigin(self &&) = delete;
    KOrigin &operator=(const self &) = delete;
    KOrigin &operator=(self &&) = delete;

    friend OriginsSetter<K, L>;
    friend CtxTrait<KOrigin<K, L>>;
    template<uint32_t, uint32_t> friend struct KOriginContextEvolveImpl;
};

// Helper for context evolution - L==1 case
template <uint32_t K, uint32_t L>
struct KOriginContextEvolveImpl {
    static const KOrigin<K, L> *evolve(const KOrigin<K, L> *prevCtx, const llvm::Instruction *I,
                                       std::unordered_set<KOrigin<K, L>> &ctxSet) {
        if (KOrigin<K, L>::callback(prevCtx, I)) {
            auto result = ctxSet.emplace(prevCtx, I);
            return &*result.first;
        }
        return prevCtx;
    }
};

// Specialization for L!=1 (not supported)
template <uint32_t K>
struct KOriginContextEvolveImpl<K, 0> {
    static const KOrigin<K, 0> *evolve(const KOrigin<K, 0> *, const llvm::Instruction *,
                                       std::unordered_set<KOrigin<K, 0>> &) {
        llvm_unreachable("L must be 1 for KOrigin");
    }
};

template <uint32_t K, uint32_t L>
struct CtxTrait<KOrigin<K, L>> {
private:
    static const KOrigin<K, L> initCtx;
    static const KOrigin<K, L> globCtx;
    static std::unordered_set<KOrigin<K, L>> ctxSet;

public:
    static const KOrigin<K, L> *contextEvolve(const KOrigin<K, L> *prevCtx, const llvm::Instruction *I) {
        static_assert(L == 1, "KOrigin only supports L=1 currently");
        return KOriginContextEvolveImpl<K, L>::evolve(prevCtx, I, ctxSet);
    }

    static const KOrigin<K, L> *getInitialCtx() { return &initCtx; }

    static const KOrigin<K, L> *getGlobalCtx() { return &globCtx; }

    // 3rd, string representation
    static std::string toString(const KOrigin<K, L> *context, bool detailed = false) {
        if (context == &globCtx) return "<global>";
        if (context == &initCtx) return "<empty>";
        return context->toString(detailed);
    }

    static void release() {
        llvm::outs() << "number of origin " << ctxSet.size() << "\n";
        ctxSet.clear();
    }
};

template <uint32_t K, uint32_t L>
const KOrigin<K, L> CtxTrait<KOrigin<K, L>>::initCtx{};

template <uint32_t K, uint32_t L>
const KOrigin<K, L> CtxTrait<KOrigin<K, L>>::globCtx{};

template <uint32_t K, uint32_t L>
std::unordered_set<KOrigin<K, L>> CtxTrait<KOrigin<K, L>>::ctxSet{};

template <uint32_t K, uint32_t L>
std::function<bool(const KOrigin<K, L> *, const llvm::Instruction *)> KOrigin<K, L>::callback =
    [] (const KOrigin<K, L> *, const llvm::Instruction *) {
        // by default no function is origin
        return false;
    };

}  // namespace aser

namespace std {

// only hash context and value
template <uint32_t K, uint32_t L>
struct hash<aser::KOrigin<K, L>> {
    size_t operator()(const aser::KOrigin<K, L> &origin) const {
        return hash<aser::KCallSite<K*L>>()(origin);
    }
};

}  // namespace std

#endif  // ASER_PTA_KORIGIN_H
