#pragma once

#include <cstddef>
#include <vector>

namespace lotus::datalog {

enum class ExplainMode {
  Plan,
  Analyze,
};

struct OperationProfile {
  std::size_t invocations = 0;
  std::size_t candidate_rows = 0;
  std::size_t matched_rows = 0;
  std::size_t output_bindings = 0;
};

struct RuleProfile {
  std::size_t evaluations = 0;
  std::size_t head_candidates = 0;
  std::vector<OperationProfile> operations;
};

struct ExecutionProfile {
  bool collected = false;
  std::vector<RuleProfile> rules;
};

} // namespace lotus::datalog
