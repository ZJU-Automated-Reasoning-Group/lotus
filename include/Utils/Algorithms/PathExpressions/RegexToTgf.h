//===-- PathExpressions/RegexToTgf.h - Regex syntax tree to TGF -----------===//
//
// Migrated from Ultimate Library-PathExpressions (v0.3.1):
// de.uni_freiburg.informatik.ultimate.lib.pathexpressions.regex.RegexToTgf
//
// Converts a regex syntax tree into a string in Trivial Graph Format (TGF).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOTGF_H
#define LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOTGF_H

#include "Utils/Algorithms/PathExpressions/Regex.h"

#include <string>

namespace lotus {
namespace pathexpressions {

template <typename L>
class RegexToTgf : public IRegexVisitor<L, RegexToTgf<L> &, std::nullptr_t> {
public:
  static std::string apply(const RegexRef<L> &regex) {
    RegexToTgf<L> v;
    regex->accept(v, nullptr);
    return v.toString();
  }

  RegexToTgf<L> &visit(const Union<L> &re, std::nullptr_t) override {
    const int thisId = addNode("∪");
    addLeftEdge(thisId, nextNodeId_);
    re.getFirst()->accept(*this, nullptr);
    addRightEdge(thisId, nextNodeId_);
    re.getSecond()->accept(*this, nullptr);
    return *this;
  }

  RegexToTgf<L> &visit(const Concatenation<L> &re, std::nullptr_t) override {
    const int thisId = addNode("·");
    addLeftEdge(thisId, nextNodeId_);
    re.getFirst()->accept(*this, nullptr);
    addRightEdge(thisId, nextNodeId_);
    re.getSecond()->accept(*this, nullptr);
    return *this;
  }

  RegexToTgf<L> &visit(const Star<L> &re, std::nullptr_t) override {
    const int thisId = addNode("*");
    addLeftEdge(thisId, nextNodeId_);
    re.getInner()->accept(*this, nullptr);
    return *this;
  }

  RegexToTgf<L> &visit(const Literal<L> &re, std::nullptr_t) override {
    addNode(re.toString());
    return *this;
  }

  RegexToTgf<L> &visit(const Epsilon<L> &, std::nullptr_t) override {
    addNode("ε");
    return *this;
  }

  RegexToTgf<L> &visit(const EmptySet<L> &, std::nullptr_t) override {
    addNode("∅");
    return *this;
  }

  std::string toString() const { return nodeList_ + "#\n" + edgeList_; }

private:
  int nextNodeId_ = 0;
  std::string nodeList_;
  std::string edgeList_;

  int addNode(const std::string &label) {
    nodeList_ += std::to_string(nextNodeId_) + " " + label + "\n";
    return nextNodeId_++;
  }

  void addEdge(const int sourceId, const int targetId,
               const std::string &label) {
    edgeList_ += std::to_string(sourceId) + " " + std::to_string(targetId) +
                 " " + label + "\n";
  }

  void addLeftEdge(const int sourceId, const int targetId) {
    addEdge(sourceId, targetId, "0");
  }

  void addRightEdge(const int sourceId, const int targetId) {
    addEdge(sourceId, targetId, "1");
  }
};

} // namespace pathexpressions
} // namespace lotus

#endif // LOTUS_UTILS_GENERAL_PATHEXPRESSIONS_REGEXTOTGF_H
