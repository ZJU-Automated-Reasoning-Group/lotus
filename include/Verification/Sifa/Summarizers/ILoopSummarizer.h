//===-- Verification/Sifa/Summarizers/ILoopSummarizer.h -------------------===//
//
// Loop summarization interface (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan..."): the loop summarization operator
// computes a summary for the Kleene-star (re)*. DagInterpreter delegates
// Star nodes to the loop summarizer (FixpointLoopSummarizer).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H

#include "Utils/Algorithms/PathExpressions/Regex.h"

namespace lotus {
namespace sifa {

template <typename L, typename StateT> class ILoopSummarizer {
public:
  virtual ~ILoopSummarizer() = default;
  virtual StateT summarize(const lotus::pathexpressions::Star<L> &star,
                           const StateT &input) = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_ILOOPSUMMARIZER_H
