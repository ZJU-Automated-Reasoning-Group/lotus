#pragma once

#include "Alias/TPA/PointerAnalysis/Program/CFG/NodeVisitor.h"

namespace llvm {
class raw_ostream;
} // namespace llvm

namespace util {
namespace io {

template <typename T>
class NodePrinter : public tpa::ConstNodeVisitor<NodePrinter<T>> {
private:
  llvm::raw_ostream &os;

public:
  NodePrinter(llvm::raw_ostream &);

  void visitEntryNode(const tpa::EntryNodeMixin<T> &);
  void visitAllocNode(const tpa::AllocNodeMixin<T> &);
  void visitCopyNode(const tpa::CopyNodeMixin<T> &);
  void visitOffsetNode(const tpa::OffsetNodeMixin<T> &);
  void visitLoadNode(const tpa::LoadNodeMixin<T> &);
  void visitStoreNode(const tpa::StoreNodeMixin<T> &);
  void visitCallNode(const tpa::CallNodeMixin<T> &);
  void visitReturnNode(const tpa::ReturnNodeMixin<T> &);
};

} // namespace io
} // namespace util