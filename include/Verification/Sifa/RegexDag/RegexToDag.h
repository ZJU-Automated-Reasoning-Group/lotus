//===-- Verification/Sifa/RegexDag/RegexToDag.h ---------------------------===//
//
// Convert a PathExpressions regex into a RegexDag (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXTODAG_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXTODAG_H

#include "Verification/Sifa/RegexDag/RegexDag.h"

namespace lotus {
namespace sifa {

template <typename L>
class RegexToDag final
    : public lotus::pathexpressions::IRegexVisitor<L, RegexDagNode<L> *,
                                                   RegexDagNode<L> *> {
public:
  using Node = RegexDagNode<L>;
  using Dag = RegexDag<L>;
  using RegexRef = lotus::pathexpressions::RegexRef<L>;

  RegexToDag() { resetDag(); }

  Node *add(const RegexRef &regex) {
    // Works only as long as source and sink are epsilon and dag isn't modified.
    Node *regexSink = regex->accept(*this, dag_.getSource());
    regexSink->connectOutgoing(dag_.getSink());
    return regexSink;
  }

  Dag getDagAndReset() {
    Dag result = std::move(dag_);
    resetDag();
    return result;
  }

  Node *visit(const lotus::pathexpressions::Union<L> &re,
              Node *parent) override {
    Node *join = dag_.makeEpsilonNode();
    join->connectIncoming(re.getFirst()->accept(*this, parent));
    join->connectIncoming(re.getSecond()->accept(*this, parent));
    return join;
  }

  Node *visit(const lotus::pathexpressions::Concatenation<L> &re,
              Node *parent) override {
    return re.getSecond()->accept(*this, re.getFirst()->accept(*this, parent));
  }

  Node *visit(const lotus::pathexpressions::Star<L> &re,
              Node *parent) override {
    return appendAsNode(lotus::pathexpressions::Regex<L>::star(re.getInner()),
                        parent);
  }

  Node *visit(const lotus::pathexpressions::Literal<L> &re,
              Node *parent) override {
    return appendAsNode(
        lotus::pathexpressions::Regex<L>::literal(re.getLetter()), parent);
  }

  Node *visit(const lotus::pathexpressions::Epsilon<L> &re,
              Node *parent) override {
    (void)re;
    return parent;
  }

  Node *visit(const lotus::pathexpressions::EmptySet<L> &re,
              Node *parent) override {
    (void)re;
    return appendAsNode(lotus::pathexpressions::Regex<L>::emptySet(), parent);
  }

private:
  void resetDag() {
    dag_ = Dag{};
    Node *src = dag_.makeEpsilonNode();
    Node *sink = dag_.makeEpsilonNode();
    dag_.setSource(src);
    dag_.setSink(sink);
  }

  Node *appendAsNode(RegexRef content, Node *parent) {
    Node *newSink = dag_.makeNode(std::move(content));
    parent->connectOutgoing(newSink);
    return newSink;
  }

  Dag dag_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXTODAG_H
