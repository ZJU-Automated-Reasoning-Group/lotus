#pragma once

#include "Alias/TPA/PointerAnalysis/Program/CFG/NodeMixin.h"

namespace tpa {

template <typename SubClass, typename RetType = void> class NodeVisitor {
public:
  template <typename NodeType> RetType visit(NodeType &node) {
    switch (node.getNodeTag()) {
    case CFGNodeTag::Entry:
      return static_cast<SubClass *>(this)->visitEntryNode(
          static_cast<EntryNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Alloc:
      return static_cast<SubClass *>(this)->visitAllocNode(
          static_cast<AllocNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Copy:
      return static_cast<SubClass *>(this)->visitCopyNode(
          static_cast<CopyNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Offset:
      return static_cast<SubClass *>(this)->visitOffsetNode(
          static_cast<OffsetNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Load:
      return static_cast<SubClass *>(this)->visitLoadNode(
          static_cast<LoadNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Store:
      return static_cast<SubClass *>(this)->visitStoreNode(
          static_cast<StoreNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Call:
      return static_cast<SubClass *>(this)->visitCallNode(
          static_cast<CallNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Ret:
      return static_cast<SubClass *>(this)->visitReturnNode(
          static_cast<ReturnNodeMixin<NodeType> &>(node));
    }
  }
};

template <typename SubClass, typename RetType = void> class ConstNodeVisitor {
public:
  template <typename NodeType> RetType visit(const NodeType &node) {
    switch (node.getNodeTag()) {
    case CFGNodeTag::Entry:
      return static_cast<SubClass *>(this)->visitEntryNode(
          static_cast<const EntryNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Alloc:
      return static_cast<SubClass *>(this)->visitAllocNode(
          static_cast<const AllocNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Copy:
      return static_cast<SubClass *>(this)->visitCopyNode(
          static_cast<const CopyNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Offset:
      return static_cast<SubClass *>(this)->visitOffsetNode(
          static_cast<const OffsetNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Load:
      return static_cast<SubClass *>(this)->visitLoadNode(
          static_cast<const LoadNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Store:
      return static_cast<SubClass *>(this)->visitStoreNode(
          static_cast<const StoreNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Call:
      return static_cast<SubClass *>(this)->visitCallNode(
          static_cast<const CallNodeMixin<NodeType> &>(node));
    case CFGNodeTag::Ret:
      return static_cast<SubClass *>(this)->visitReturnNode(
          static_cast<const ReturnNodeMixin<NodeType> &>(node));
    }
  }
};

} // namespace tpa