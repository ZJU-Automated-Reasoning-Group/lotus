//===-- Verification/Sifa/Statistics/SifaStats.cpp
//-------------------------===//
//
// Implementation of SifaStats (Ultimate-aligned).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Statistics/SifaStats.h"

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>

namespace lotus {
namespace sifa {

std::size_t SifaStats::KeyHash::operator()(Key k) const {
  return static_cast<std::size_t>(k);
}

namespace {

const char *keyToString(SifaStats::Key k) {
  switch (k) {
#define SIFA_KEY(x)                                                            \
  case SifaStats::Key::x:                                                      \
    return #x;
    SIFA_KEY(OVERALL_TIME)
    SIFA_KEY(ICFG_INTERPRETER_ENTERED_PROCEDURES)
    SIFA_KEY(DAG_INTERPRETER_EARLY_EXIT_QUERIES_NONTRIVIAL)
    SIFA_KEY(DAG_INTERPRETER_EARLY_EXITS)
    SIFA_KEY(TOOLS_POST_APPLICATIONS)
    SIFA_KEY(TOOLS_POST_TIME)
    SIFA_KEY(TOOLS_POST_CALL_APPLICATIONS)
    SIFA_KEY(TOOLS_POST_CALL_TIME)
    SIFA_KEY(TOOLS_POST_RETURN_APPLICATIONS)
    SIFA_KEY(TOOLS_POST_RETURN_TIME)
    SIFA_KEY(TOOLS_QUANTIFIERELIM_APPLICATIONS)
    SIFA_KEY(TOOLS_QUANTIFIERELIM_TIME)
    SIFA_KEY(TOOLS_QUANTIFIERELIM_MAX_TIME)
    SIFA_KEY(FLUID_QUERY_TIME)
    SIFA_KEY(FLUID_QUERIES)
    SIFA_KEY(FLUID_YES_ANSWERS)
    SIFA_KEY(DOMAIN_JOIN_APPLICATIONS)
    SIFA_KEY(DOMAIN_JOIN_TIME)
    SIFA_KEY(DOMAIN_ALPHA_APPLICATIONS)
    SIFA_KEY(DOMAIN_ALPHA_TIME)
    SIFA_KEY(DOMAIN_WIDEN_APPLICATIONS)
    SIFA_KEY(DOMAIN_WIDEN_TIME)
    SIFA_KEY(DOMAIN_ISSUBSETEQ_APPLICATIONS)
    SIFA_KEY(DOMAIN_ISSUBSETEQ_TIME)
    SIFA_KEY(DOMAIN_ISBOTTOM_APPLICATIONS)
    SIFA_KEY(DOMAIN_ISBOTTOM_TIME)
    SIFA_KEY(LOOP_SUMMARIZER_APPLICATIONS)
    SIFA_KEY(LOOP_SUMMARIZER_CACHE_MISSES)
    SIFA_KEY(LOOP_SUMMARIZER_OVERALL_TIME)
    SIFA_KEY(LOOP_SUMMARIZER_NEW_COMPUTATION_TIME)
    SIFA_KEY(LOOP_SUMMARIZER_FIXPOINT_ITERATIONS)
    SIFA_KEY(CALL_SUMMARIZER_APPLICATIONS)
    SIFA_KEY(CALL_SUMMARIZER_CACHE_MISSES)
    SIFA_KEY(CALL_SUMMARIZER_OVERALL_TIME)
    SIFA_KEY(CALL_SUMMARIZER_NEW_COMPUTATION_TIME)
    SIFA_KEY(PROCEDURE_GRAPH_BUILDER_TIME)
    SIFA_KEY(PATH_EXPR_TIME)
    SIFA_KEY(REGEX_TO_DAG_TIME)
    SIFA_KEY(DAG_COMPRESSION_TIME)
    SIFA_KEY(DAG_COMPRESSION_PROCESSED_NODES)
    SIFA_KEY(DAG_COMPRESSION_RETAINED_NODES)
    SIFA_KEY(FLUID_ABSTRACTIONS_APPLIED)
#undef SIFA_KEY
  }
  return "";
}

SifaStats::Key stringToKey(const std::string &s) {
#define SIFA_KEY(x)                                                            \
  if (s == #x)                                                                 \
    return SifaStats::Key::x;
  SIFA_KEY(OVERALL_TIME)
  SIFA_KEY(ICFG_INTERPRETER_ENTERED_PROCEDURES)
  SIFA_KEY(DAG_INTERPRETER_EARLY_EXIT_QUERIES_NONTRIVIAL)
  SIFA_KEY(DAG_INTERPRETER_EARLY_EXITS)
  SIFA_KEY(TOOLS_POST_APPLICATIONS)
  SIFA_KEY(TOOLS_POST_TIME)
  SIFA_KEY(TOOLS_POST_CALL_APPLICATIONS)
  SIFA_KEY(TOOLS_POST_CALL_TIME)
  SIFA_KEY(TOOLS_POST_RETURN_APPLICATIONS)
  SIFA_KEY(TOOLS_POST_RETURN_TIME)
  SIFA_KEY(TOOLS_QUANTIFIERELIM_APPLICATIONS)
  SIFA_KEY(TOOLS_QUANTIFIERELIM_TIME)
  SIFA_KEY(TOOLS_QUANTIFIERELIM_MAX_TIME)
  SIFA_KEY(FLUID_QUERY_TIME)
  SIFA_KEY(FLUID_QUERIES)
  SIFA_KEY(FLUID_YES_ANSWERS)
  SIFA_KEY(DOMAIN_JOIN_APPLICATIONS)
  SIFA_KEY(DOMAIN_JOIN_TIME)
  SIFA_KEY(DOMAIN_ALPHA_APPLICATIONS)
  SIFA_KEY(DOMAIN_ALPHA_TIME)
  SIFA_KEY(DOMAIN_WIDEN_APPLICATIONS)
  SIFA_KEY(DOMAIN_WIDEN_TIME)
  SIFA_KEY(DOMAIN_ISSUBSETEQ_APPLICATIONS)
  SIFA_KEY(DOMAIN_ISSUBSETEQ_TIME)
  SIFA_KEY(DOMAIN_ISBOTTOM_APPLICATIONS)
  SIFA_KEY(DOMAIN_ISBOTTOM_TIME)
  SIFA_KEY(LOOP_SUMMARIZER_APPLICATIONS)
  SIFA_KEY(LOOP_SUMMARIZER_CACHE_MISSES)
  SIFA_KEY(LOOP_SUMMARIZER_OVERALL_TIME)
  SIFA_KEY(LOOP_SUMMARIZER_NEW_COMPUTATION_TIME)
  SIFA_KEY(LOOP_SUMMARIZER_FIXPOINT_ITERATIONS)
  SIFA_KEY(CALL_SUMMARIZER_APPLICATIONS)
  SIFA_KEY(CALL_SUMMARIZER_CACHE_MISSES)
  SIFA_KEY(CALL_SUMMARIZER_OVERALL_TIME)
  SIFA_KEY(CALL_SUMMARIZER_NEW_COMPUTATION_TIME)
  SIFA_KEY(PROCEDURE_GRAPH_BUILDER_TIME)
  SIFA_KEY(PATH_EXPR_TIME)
  SIFA_KEY(REGEX_TO_DAG_TIME)
  SIFA_KEY(DAG_COMPRESSION_TIME)
  SIFA_KEY(DAG_COMPRESSION_PROCESSED_NODES)
  SIFA_KEY(DAG_COMPRESSION_RETAINED_NODES)
  SIFA_KEY(FLUID_ABSTRACTIONS_APPLIED)
#undef SIFA_KEY
  throw std::invalid_argument("Unknown SifaStats key: " + s);
}

} // namespace

bool SifaStats::keyIsStopwatch(Key k) {
  switch (k) {
  case Key::OVERALL_TIME:
  case Key::TOOLS_POST_TIME:
  case Key::TOOLS_POST_CALL_TIME:
  case Key::TOOLS_POST_RETURN_TIME:
  case Key::TOOLS_QUANTIFIERELIM_TIME:
  case Key::TOOLS_QUANTIFIERELIM_MAX_TIME:
  case Key::FLUID_QUERY_TIME:
  case Key::DOMAIN_JOIN_TIME:
  case Key::DOMAIN_ALPHA_TIME:
  case Key::DOMAIN_WIDEN_TIME:
  case Key::DOMAIN_ISSUBSETEQ_TIME:
  case Key::DOMAIN_ISBOTTOM_TIME:
  case Key::LOOP_SUMMARIZER_OVERALL_TIME:
  case Key::LOOP_SUMMARIZER_NEW_COMPUTATION_TIME:
  case Key::CALL_SUMMARIZER_OVERALL_TIME:
  case Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME:
  case Key::PROCEDURE_GRAPH_BUILDER_TIME:
  case Key::PATH_EXPR_TIME:
  case Key::REGEX_TO_DAG_TIME:
  case Key::DAG_COMPRESSION_TIME:
    return true;
  default:
    return false;
  }
}

std::uint64_t SifaStats::getValue(const std::string &keyName) const {
  return getValue(stringToKey(keyName));
}

std::uint64_t SifaStats::getValue(Key k) const {
  if (keyIsStopwatch(k)) {
    if (k == Key::TOOLS_QUANTIFIERELIM_MAX_TIME)
      return static_cast<std::uint64_t>(maxDuration(k).count());
    return static_cast<std::uint64_t>(duration(k).count());
  }
  return counter(k);
}

std::vector<std::string> SifaStats::getKeys() const {
  std::vector<std::string> out;
  for (int i = 0; i < static_cast<int>(Key::FLUID_ABSTRACTIONS_APPLIED) + 1;
       ++i) {
    Key k = static_cast<Key>(i);
    const char *n = keyToString(k);
    if (n && *n)
      out.push_back(n);
  }
  return out;
}

std::vector<std::string> SifaStats::getStopwatches() const {
  std::vector<std::string> out;
  for (int i = 0; i < static_cast<int>(Key::FLUID_ABSTRACTIONS_APPLIED) + 1;
       ++i) {
    Key k = static_cast<Key>(i);
    if (keyIsStopwatch(k)) {
      const char *n = keyToString(k);
      if (n && *n)
        out.push_back(n);
    }
  }
  return out;
}

void SifaStats::increment(Key k, std::uint64_t by) { counters_[k] += by; }

void SifaStats::add(Key k, std::uint64_t summand) { counters_[k] += summand; }

void SifaStats::start(Key k) {
  const int level = ++stopwatchNesting_[k];
  if (level == 1) {
    starts_[k] = Clock::now();
  }
}

void SifaStats::stop(Key k) {
  auto it = stopwatchNesting_.find(k);
  if (it == stopwatchNesting_.end() || it->second < 1) {
    return;
  }
  const int level = --it->second;
  if (level == 0) {
    auto startIt = starts_.find(k);
    if (startIt != starts_.end()) {
      durations_[k] += std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - startIt->second);
    }
  }
}

void SifaStats::startMax(Key k) {
  const int level = ++stopwatchNesting_[k];
  if (level == 1) {
    maxStarts_[k] = Clock::now();
  }
}

void SifaStats::stopMax(Key k) {
  auto it = stopwatchNesting_.find(k);
  if (it == stopwatchNesting_.end() || it->second < 1) {
    return;
  }
  const int level = --it->second;
  if (level == 0) {
    auto startIt = maxStarts_.find(k);
    if (startIt != maxStarts_.end()) {
      auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now() - startIt->second);
      durations_[k] += elapsed;
      auto &maxVal = maxDurations_[k];
      if (elapsed > maxVal) {
        maxVal = elapsed;
      }
    }
  }
}

std::uint64_t SifaStats::counter(Key k) const {
  auto it = counters_.find(k);
  return it == counters_.end() ? 0 : it->second;
}

std::chrono::nanoseconds SifaStats::duration(Key k) const {
  auto it = durations_.find(k);
  return it == durations_.end() ? std::chrono::nanoseconds(0) : it->second;
}

std::chrono::nanoseconds SifaStats::maxDuration(Key k) const {
  auto it = maxDurations_.find(k);
  return it == maxDurations_.end() ? std::chrono::nanoseconds(0) : it->second;
}

} // namespace sifa
} // namespace lotus
