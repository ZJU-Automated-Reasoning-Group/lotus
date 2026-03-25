//===-- Verification/Sifa/RegexDag/RegexDagUtils.h ------------------------===//
//
// Utility functions for RegexDag overlays and marking (ported from Ultimate
// Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGUTILS_H
#define LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGUTILS_H

#include "Utils/Algorithms/PathExpressions/Regex.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/RegexDag/IDagOverlay.h"

#include <functional>
#include <queue>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace lotus {
namespace sifa {

// Loop point visitor (Ultimate's LoopPointVisitor): returns the loop point
// (BasicBlock*) of a starred regex over Transition labels.
class LoopPointVisitorTransition final
    : public lotus::pathexpressions::IRegexVisitor<
          Transition, const llvm::BasicBlock *, void *> {
public:
  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::Star<Transition> &re,
        void *arg) override {
    (void)arg;
    return re.getInner()->accept(*this, (void *)nullptr);
  }

  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::Union<Transition> &re,
        void *arg) override {
    (void)arg;
    const llvm::BasicBlock *a = re.getFirst()->accept(*this, (void *)nullptr);
    const llvm::BasicBlock *b = re.getSecond()->accept(*this, (void *)nullptr);
    if (a != b) {
      throw std::logic_error("Loop points differ");
    }
    return a;
  }

  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::Concatenation<Transition> &re,
        void *arg) override {
    (void)arg;
    const llvm::BasicBlock *a = re.getFirst()->accept(*this, (void *)nullptr);
    if (a) {
      return a;
    }
    return re.getSecond()->accept(*this, (void *)nullptr);
  }

  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::Literal<Transition> &re,
        void *arg) override {
    (void)arg;
    return re.getLetter().source;
  }

  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::Epsilon<Transition> &re,
        void *arg) override {
    (void)re;
    (void)arg;
    return nullptr;
  }

  const llvm::BasicBlock *
  visit(const lotus::pathexpressions::EmptySet<Transition> &re,
        void *arg) override {
    (void)re;
    (void)arg;
    throw std::invalid_argument("Loop contained empty set");
  }
};

/// Mark a regex by appending a unique marker literal.
inline lotus::pathexpressions::RegexRef<Transition>
markRegex(const lotus::pathexpressions::RegexRef<Transition> &regex,
          const llvm::BasicBlock *finalLocationAsMark, std::uint32_t markerId) {
  const auto marker = lotus::pathexpressions::Regex<Transition>::literal(
      Transition::makeMarker(markerId, finalLocationAsMark));
  return lotus::pathexpressions::Regex<Transition>::concat(regex, marker);
}

template <typename T> static T getOnly(const std::unordered_set<T> &s) {
  if (s.size() != 1) {
    throw std::invalid_argument("Expected exactly one element");
  }
  return *s.begin();
}

static std::unordered_set<const llvm::BasicBlock *> nextLocationsTransition(
    const std::vector<RegexDagNode<Transition> *> &startPoints,
    const std::function<const llvm::BasicBlock *(const Transition &)>
        &getLocInStartDir,
    const std::function<std::vector<RegexDagNode<Transition> *>(
        RegexDagNode<Transition> *)> &nextNodes) {
  std::unordered_set<const llvm::BasicBlock *> result;
  std::queue<RegexDagNode<Transition> *> q;
  for (auto *n : startPoints)
    q.push(n);

  while (!q.empty()) {
    auto *node = q.front();
    q.pop();
    const auto &regex = node->getContent();
    if (dynamic_cast<const lotus::pathexpressions::Literal<Transition> *>(
            regex.get())) {
      const auto *lit =
          static_cast<const lotus::pathexpressions::Literal<Transition> *>(
              regex.get());
      result.insert(getLocInStartDir(lit->getLetter()));
    } else if (dynamic_cast<const lotus::pathexpressions::Star<Transition> *>(
                   regex.get())) {
      LoopPointVisitorTransition v;
      result.insert(regex->accept(v, (void *)nullptr));
    } else if (regex->isEpsilon() || regex->isEmptySet()) {
      for (auto *n : nextNodes(node))
        q.push(n);
    } else {
      throw std::logic_error("Illegal regex type in RegexDag");
    }
  }
  return result;
}

inline const llvm::BasicBlock *
singleSinkLocation(const RegexDag<Transition> &dag,
                   const IDagOverlay<Transition> &overlay) {
  const auto locs = nextLocationsTransition(
      overlay.sinks(dag), [](const Transition &t) { return t.target; },
      [&](auto *n) { return overlay.predecessorsOf(n); });
  return getOnly(locs);
}

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_REGEXDAG_REGEXDAGUTILS_H
