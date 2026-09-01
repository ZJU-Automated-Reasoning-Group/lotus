//
// Created by peiming on 11/2/19.
//
#ifndef ASER_PTA_CGPTRNODE_H
#define ASER_PTA_CGPTRNODE_H

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/CtxTrait.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/CGNodeBase.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Program/Pointer.h"

namespace aser {

template <typename ctx> class CallGraphNode;
template <typename ctx> class ConstraintGraph;

// nodes represent pointers
template <typename ctx> class CGPtrNode : public CGNodeBase<ctx> {
protected:
  using T = Pointer<ctx>;
  using super = CGNodeBase<ctx>;
  const T *ptr;

  std::unique_ptr<llvm::SparseBitVector<5120>> cachedPts;

  CGPtrNode(const T *ptr, NodeID id)
      : super(id, CGNodeKind::PtrNode), ptr(ptr), cachedPts() {}

  // for anonmyous ptrnode
  explicit CGPtrNode(NodeID id)
      : super(id, CGNodeKind::PtrNode), ptr(nullptr), cachedPts() {}

public:
  static inline bool classof(const super *node) {
    return node->getType() == CGNodeKind::PtrNode;
  }

  inline bool isAnonNode() const { return getPointer() == nullptr; }

  inline const T *getPointer() const { return ptr; }

  inline const ctx *getContext() const {
    return this->getPointer()->getContext();
  }

  inline void cachePts(const llvm::SparseBitVector<5120> &pts) {
    cachedPts.reset(new llvm::SparseBitVector<5120>(pts));
  }

  llvm::SparseBitVector<5120> *getCachedPts() const { return cachedPts.get(); }

  __attribute__((warn_unused_result)) std::string toString() const override {
    std::string str;
    llvm::raw_string_ostream os(str);
    if (this->isSuperNode()) {
      os << "SuperNode: \n";
      llvm::dump(this->childNodes, os);
    } else {
      os << super::getNodeID() << "\n";
      if (ptr == nullptr) {
        os << "anonymous ptr";
      } else if (ptr->getValue()->hasName()) {
        os << CtxTrait<ctx>::toString(getContext(), true) << "\n";
        os << *ptr->getValue() << "\n"; //->getName();
      } else {
        os << CtxTrait<ctx>::toString(getContext(), true) << "\n";
        os << *ptr->getValue() << "\n";
      }
    }
    return os.str();
  }

  friend class GraphBase<super, Constraints>;
  friend class ConstraintGraph<ctx>;
};

} // namespace aser

#endif
