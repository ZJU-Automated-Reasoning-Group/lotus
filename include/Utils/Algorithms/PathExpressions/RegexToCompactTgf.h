//===-- PathExpressions/RegexToCompactTgf.h - Compressed regex TGF --------===//
//
// Migrated from Ultimate Library-PathExpressions (v0.3.1):
// de.uni_freiburg.informatik.ultimate.lib.pathexpressions.regex.RegexToCompactTgf
//
// Like RegexToTgf, but compresses nested unions/concatenations into a single
// node with multiple children (lossy w.r.t. parenthesization).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOCOMPACTTGF_H
#define LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOCOMPACTTGF_H

#include "Utils/Algorithms/PathExpressions/Regex.h"

#include <string>
#include <typeindex>

namespace lotus {
namespace pathexpressions {

namespace detail {
template <typename L> struct RegexToCompactTgfArg {
  RegexToCompactTgfArg(const int parentId, const IRegex<L> &parent)
      : parentId_(parentId), parentType_(std::type_index(typeid(parent))) {}
  int parentId_;
  std::type_index parentType_;
  int childOffset_ = 0;
};
} // namespace detail

template <typename L>
class RegexToCompactTgf
    : public IRegexVisitor<L, RegexToCompactTgf<L> &,
                           detail::RegexToCompactTgfArg<L> *> {
public:
  using Arg = detail::RegexToCompactTgfArg<L>;

  static std::string apply(const RegexRef<L> &regex) {
    RegexToCompactTgf<L> v;
    regex->accept(v, static_cast<Arg *>(nullptr));
    return v.toString();
  }

  RegexToCompactTgf<L> &visit(const Union<L> &re, Arg *arg) override {
    return visitAndCompact("∪", re, arg, re.getFirst(), re.getSecond());
  }

  RegexToCompactTgf<L> &visit(const Concatenation<L> &re, Arg *arg) override {
    return visitAndCompact("·", re, arg, re.getFirst(), re.getSecond());
  }

  RegexToCompactTgf<L> &visit(const Star<L> &re, Arg *arg) override {
    const int starId = addNodeLinkedToParent("*", arg);
    Arg childArg(starId, re);
    re.getInner()->accept(*this, &childArg);
    return *this;
  }

  RegexToCompactTgf<L> &visit(const Literal<L> &re, Arg *arg) override {
    addNodeLinkedToParent(re.toString(), arg);
    return *this;
  }

  RegexToCompactTgf<L> &visit(const Epsilon<L> &, Arg *arg) override {
    addNodeLinkedToParent("ε", arg);
    return *this;
  }

  RegexToCompactTgf<L> &visit(const EmptySet<L> &, Arg *arg) override {
    addNodeLinkedToParent("∅", arg);
    return *this;
  }

  std::string toString() const { return nodeList_ + "#\n" + edgeList_; }

private:
  int nextNodeId_ = 0;
  std::string nodeList_;
  std::string edgeList_;

  void addEdge(const int sourceId, const int targetId, const int label) {
    edgeList_ += std::to_string(sourceId) + " " + std::to_string(targetId) +
                 " " + std::to_string(label) + "\n";
  }

  void linkNextNodeToParent(Arg *arg) {
    if (!arg)
      return;
    addEdge(arg->parentId_, nextNodeId_, arg->childOffset_);
    ++arg->childOffset_;
  }

  int addNodeLinkedToParent(const std::string &label, Arg *arg) {
    linkNextNodeToParent(arg);
    nodeList_ += std::to_string(nextNodeId_) + " " + label + "\n";
    return nextNodeId_++;
  }

  RegexToCompactTgf<L> &visitAndCompact(const std::string &nodeLabel,
                                        const IRegex<L> &node, Arg *arg,
                                        const RegexRef<L> &leftChild,
                                        const RegexRef<L> &rightChild) {
    Arg childArg(0, node);
    Arg *childArgPtr = arg;
    if (!arg || std::type_index(typeid(node)) != arg->parentType_) {
      const int nodeId = addNodeLinkedToParent(nodeLabel, arg);
      childArg = Arg(nodeId, node);
      childArgPtr = &childArg;
    }

    leftChild->accept(*this, childArgPtr);
    rightChild->accept(*this, childArgPtr);
    return *this;
  }
};

} // namespace pathexpressions
} // namespace lotus

#endif // LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOCOMPACTTGF_H
