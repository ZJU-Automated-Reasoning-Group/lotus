#include "lotus_taint_microbench.h"

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <Dataflow/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Solvers/IFDSSolver.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace {

struct FlowPair {
  unsigned source_line = 0;
  unsigned sink_line = 0;

  bool operator<(const FlowPair &other) const {
    if (source_line != other.source_line)
      return source_line < other.source_line;
    return sink_line < other.sink_line;
  }
};

std::string trim(const std::string &input) {
  size_t start = input.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = input.find_last_not_of(" \t\r\n");
  return input.substr(start, end - start + 1);
}

std::set<FlowPair> parseExpectedFlows(const std::string &expected_path) {
  std::set<FlowPair> expected;
  if (expected_path.empty())
    return expected;

  std::ifstream input(expected_path);
  if (!input.is_open()) {
    llvm::errs() << "Warning: Could not open expected file: " << expected_path
                 << "\n";
    return expected;
  }

  std::string line;
  while (std::getline(input, line)) {
    auto comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line = line.substr(0, comment_pos);
    }
    line = trim(line);
    if (line.empty())
      continue;

    auto colon_pos = line.find(':');
    if (colon_pos == std::string::npos)
      continue;

    std::string source_part = trim(line.substr(0, colon_pos));
    std::string sink_part = trim(line.substr(colon_pos + 1));
    if (source_part.empty() || sink_part.empty())
      continue;

    try {
      unsigned source_line = static_cast<unsigned>(std::stoul(source_part));
      unsigned sink_line = static_cast<unsigned>(std::stoul(sink_part));
      expected.insert({source_line, sink_line});
    } catch (const std::exception &) {
      llvm::errs() << "Warning: Failed to parse expected line: " << line
                   << "\n";
    }
  }

  return expected;
}

std::set<FlowPair>
collectPredictedFlows(const ifds::TaintAnalysis &analysis,
                      const ifds::IFDSSolver<ifds::TaintAnalysis> &solver) {
  std::set<FlowPair> predicted;

  auto append_source_line = [](std::vector<const llvm::Instruction *> &sources,
                               const llvm::Instruction *inst) {
    if (!inst)
      return;
    sources.push_back(inst);
  };

  auto results = solver.get_all_results();
  for (const auto &pair : results) {
    const auto &node = pair.first;
    const auto &facts = pair.second;

    if (facts.empty() || !node.instruction)
      continue;

    auto *call = llvm::dyn_cast<llvm::CallInst>(node.instruction);
    if (!call || !analysis.is_sink(call))
      continue;

    unsigned sink_line = call->getDebugLoc().getLine();
    if (sink_line == 0)
      continue;

    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
      const llvm::Value *arg = call->getOperand(i);
      for (const auto &fact : facts) {
        if (!analysis.is_argument_tainted(arg, fact))
          continue;

        auto path =
            analysis.trace_taint_sources_summary_based(solver, call, fact);
        std::vector<const llvm::Instruction *> sources = path.sources;
        if (fact.get_source()) {
          append_source_line(sources, fact.get_source());
        }
        if (fact.is_tainted_var()) {
          append_source_line(
              sources, llvm::dyn_cast<llvm::Instruction>(fact.get_value()));
        } else if (fact.is_tainted_memory()) {
          append_source_line(sources, llvm::dyn_cast<llvm::Instruction>(
                                          fact.get_memory_location()));
        }
        if (sources.empty())
          continue;

        for (const auto *source_inst : sources) {
          if (!source_inst)
            continue;
          unsigned source_line = source_inst->getDebugLoc().getLine();
          if (source_line == 0)
            continue;
          predicted.insert({source_line, sink_line});
        }
      }
    }
  }

  return predicted;
}

} // namespace

void runMicroBenchEvaluation(
    const ifds::TaintAnalysis &analysis,
    const ifds::IFDSSolver<ifds::TaintAnalysis> &solver,
    const std::string &expected_path, bool verbose, llvm::raw_ostream &os) {
  auto predicted_flows = collectPredictedFlows(analysis, solver);
  auto expected_flows = parseExpectedFlows(expected_path);

  size_t true_positives = 0;
  for (const auto &flow : predicted_flows) {
    if (expected_flows.count(flow)) {
      ++true_positives;
    }
  }

  size_t predicted_count = predicted_flows.size();
  size_t expected_count = expected_flows.size();
  size_t false_positives = predicted_count - true_positives;
  size_t false_negatives = expected_count - true_positives;

  double precision = predicted_count == 0
                         ? 0.0
                         : static_cast<double>(true_positives) /
                               static_cast<double>(predicted_count);
  double recall = expected_count == 0 ? 0.0
                                      : static_cast<double>(true_positives) /
                                            static_cast<double>(expected_count);

  os << "\nMicro-benchmark evaluation:\n";
  os << "===========================\n";
  os << "Expected flows: " << expected_count << "\n";
  os << "Predicted flows: " << predicted_count << "\n";
  os << "True positives: " << true_positives << "\n";
  os << "False positives: " << false_positives << "\n";
  os << "False negatives: " << false_negatives << "\n";
  os << "Precision: " << precision << "\n";
  os << "Recall: " << recall << "\n";

  if (verbose) {
    os << "\nPredicted flows (source_line:sink_line):\n";
    for (const auto &flow : predicted_flows) {
      os << flow.source_line << ":" << flow.sink_line << "\n";
    }
  }
}
