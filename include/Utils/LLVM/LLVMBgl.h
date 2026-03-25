/**
 * \file LLVMBgl.h
 * \brief LLVM control-flow graph (CFG) adapters
 * \author Lotus Team
 *
 * This file provides lightweight adapters for iterating over CFG vertices
 * and edges without relying on Boost.
 */
#ifndef __LLVM_BGL_HPP_
#define __LLVM_BGL_HPP_

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/CFG.h"

#include <cassert>
#include <cstddef>
#include <utility>

namespace llvm {
typedef std::pair<BasicBlock *, BasicBlock *> BBPair;
namespace bgl {
struct MkOutEdgePair {
  BasicBlock *src = nullptr;
  MkOutEdgePair() = default;
  explicit MkOutEdgePair(BasicBlock *u) : src(u) {}

  BBPair operator()(BasicBlock *v) const {
    assert(src);
    return std::make_pair(src, v);
  }
};

struct MkInEdgePair {
  BasicBlock *dst = nullptr;
  MkInEdgePair() = default;
  explicit MkInEdgePair(BasicBlock *v) : dst(v) {}
  BBPair operator()(BasicBlock *u) const {
    assert(dst);
    return std::make_pair(u, dst);
  }
};

using out_edge_iterator =
    llvm::mapped_iterator<llvm::succ_iterator, MkOutEdgePair>;
using in_edge_iterator =
    llvm::mapped_iterator<llvm::pred_iterator, MkInEdgePair>;

inline llvm::iterator_range<out_edge_iterator> out_edges(BasicBlock *bb) {
  return llvm::make_range(out_edge_iterator(succ_begin(bb), MkOutEdgePair(bb)),
                          out_edge_iterator(succ_end(bb), MkOutEdgePair(bb)));
}

inline std::size_t out_degree(const BasicBlock *bb) {
  return bb->getTerminator()->getNumSuccessors();
}

inline llvm::iterator_range<in_edge_iterator> in_edges(BasicBlock *bb) {
  return llvm::make_range(in_edge_iterator(pred_begin(bb), MkInEdgePair(bb)),
                          in_edge_iterator(pred_end(bb), MkInEdgePair(bb)));
}

inline std::size_t in_degree(const BasicBlock *bb) { return bb->getNumUses(); }

inline std::size_t degree(const BasicBlock *bb) {
  return in_degree(bb) + out_degree(bb);
}

inline llvm::iterator_range<Function::iterator> vertices(Function &f) {
  return llvm::make_range(f.begin(), f.end());
}

inline llvm::iterator_range<Function::const_iterator>
vertices(const Function &f) {
  return llvm::make_range(f.begin(), f.end());
}

inline std::size_t num_vertices(const Function &f) { return f.size(); }

} // namespace bgl
} // namespace llvm

#endif
