#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Solver.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace interleaved_dyck = lotus::cfl::interleaved_dyck;
namespace unary = lotus::cfl::interleaved_dyck::unary;

namespace {

struct CommandLine {
  std::string input;
  std::string output;
  unary::Algorithm algorithm = unary::Algorithm::Adaptive;
  bool sparsify = true;
  bool add_reverse_edges = false;
  std::optional<std::size_t> shallow_threshold;
  bool stats = false;
  bool print_pairs = false;
};

void usage(std::ostream &output) {
  output
      << "usage: lotus-cfl-interleaved-dyck-unary [options] <graph.dot>\n"
         "\n"
         "Compute exact bidirected unary D1-interleaved-D1 reachability.\n"
         "\n"
         "options:\n"
         "  --algorithm NAME  adaptive (default) or fixed-counter\n"
         "  --direct         skip quotient sparsification\n"
         "  --bidirect       add missing complement reverse arcs; the\n"
         "                   result overapproximates the original graph\n"
         "  --shallow K      adaptive only: solve min(counter1,counter2) <= K\n"
         "  --stats          print construction and backend statistics\n"
         "  --print-pairs    materialize non-reflexive component pairs\n"
         "  -o FILE          write output to FILE\n"
         "  -h, --help       show this help\n";
}

std::size_t parseSize(std::string_view text, std::string_view option) {
  std::size_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid value for " + std::string(option));
  }
  return value;
}

unary::Algorithm parseAlgorithm(std::string_view name) {
  if (name == "adaptive") {
    return unary::Algorithm::Adaptive;
  }
  if (name == "fixed-counter") {
    return unary::Algorithm::FixedCounter;
  }
  throw std::invalid_argument("unknown algorithm '" + std::string(name) +
                              "' (expected adaptive or fixed-counter)");
}

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      std::exit(0);
    }
    if (argument == "--algorithm") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --algorithm");
      }
      result.algorithm = parseAlgorithm(argv[i]);
      continue;
    }
    if (argument == "--direct") {
      result.sparsify = false;
      continue;
    }
    if (argument == "--bidirect") {
      result.add_reverse_edges = true;
      continue;
    }
    if (argument == "--shallow") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --shallow");
      }
      result.shallow_threshold = parseSize(argv[i], argument);
      continue;
    }
    if (argument == "--stats") {
      result.stats = true;
      continue;
    }
    if (argument == "--print-pairs") {
      result.print_pairs = true;
      continue;
    }
    if (argument == "-o") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for -o");
      }
      result.output = argv[i];
      continue;
    }
    if (!argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
    if (!result.input.empty()) {
      throw std::invalid_argument("more than one input graph was provided");
    }
    result.input = argument;
  }
  if (result.input.empty()) {
    throw std::invalid_argument("no input graph was provided");
  }
  if (result.algorithm == unary::Algorithm::FixedCounter &&
      result.shallow_threshold) {
    throw std::invalid_argument(
        "--shallow is available only with --algorithm adaptive");
  }
  return result;
}

template <typename Result> std::size_t componentCount(const Result &result) {
  std::unordered_set<std::size_t> components;
  for (const auto &[_, component] : result.components()) {
    components.insert(component);
  }
  return components.size();
}

template <typename Result>
void printPairs(std::ostream &output, const interleaved_dyck::Graph &graph,
                const Result &result) {
  for (interleaved_dyck::Vertex source : graph.vertices()) {
    for (interleaved_dyck::Vertex target : graph.vertices()) {
      if (source != target && result.connected(source, target)) {
        output << source << ' ' << target << '\n';
      }
    }
  }
}

template <typename Result>
void printPreamble(std::ostream &output, std::string_view algorithm,
                   const Result &result) {
  output << "Algorithm: " << algorithm << '\n'
         << "Components: " << componentCount(result) << '\n'
         << "Guarantee: "
         << (result.stats().overapproximates_original
                 ? "sound overapproximation of original directed graph"
                 : "exact for original bidirected graph")
         << '\n';
}

void printAdaptiveResult(std::ostream &output, const CommandLine &command_line,
                         const interleaved_dyck::Graph &graph,
                         const unary::AdaptiveResult &result,
                         std::int64_t elapsed_ms) {
  printPreamble(
      output, command_line.shallow_threshold ? "adaptive-shallow" : "adaptive",
      result);
  if (command_line.stats) {
    const unary::AdaptiveStats &stats = result.stats();
    output << "  input vertices/arcs: " << stats.input_vertices << '/'
           << stats.input_arcs << '\n'
           << "  quotient vertices/arcs: " << stats.quotient_vertices << '/'
           << stats.quotient_arcs << '\n'
           << "  threshold: " << stats.threshold << '\n'
           << "  vertical states/arcs: " << stats.vertical_control_states << '/'
           << stats.vertical_arcs << '\n'
           << "  horizontal states/arcs: " << stats.horizontal_control_states
           << '/' << stats.horizontal_arcs << '\n'
           << "  vertical Dyck pops/unions: "
           << stats.vertical_dyck.worklist_pops << '/'
           << stats.vertical_dyck.component_unions << '\n'
           << "  horizontal Dyck pops/unions: "
           << stats.horizontal_dyck.worklist_pops << '/'
           << stats.horizontal_dyck.component_unions << '\n'
           << "  quotient Dyck pops/unions: "
           << stats.quotient_dyck.worklist_pops << '/'
           << stats.quotient_dyck.component_unions << '\n'
           << "  added reverse arcs: " << stats.added_reverse_arcs << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    printPairs(output, graph, result);
  }
}

void printFixedCounterResult(std::ostream &output,
                             const CommandLine &command_line,
                             const interleaved_dyck::Graph &graph,
                             const unary::FixedCounterResult &result,
                             std::int64_t elapsed_ms) {
  printPreamble(output, "fixed-counter", result);
  if (command_line.stats) {
    const unary::FixedCounterStats &stats = result.stats();
    output << "  input vertices/arcs: " << stats.input_vertices << '/'
           << stats.input_arcs << '\n'
           << "  quotient vertices/arcs: " << stats.quotient_vertices << '/'
           << stats.quotient_arcs << '\n'
           << "  counter bound: " << stats.counter_bound << '\n'
           << "  control states: " << stats.control_states << '\n'
           << "  translated arcs: " << stats.translated_arcs << '\n'
           << "  epsilon/closing edges: " << stats.epsilon_edges << '/'
           << stats.closing_edges << '\n'
           << "  Dyck worklist pops/unions: " << stats.dyck.worklist_pops << '/'
           << stats.dyck.component_unions << '\n'
           << "  quotient Dyck pops/unions: "
           << stats.quotient_dyck.worklist_pops << '/'
           << stats.quotient_dyck.component_unions << '\n'
           << "  added reverse arcs: " << stats.added_reverse_arcs << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    printPairs(output, graph, result);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const interleaved_dyck::Graph graph =
        interleaved_dyck::Graph::parseDotFile(command_line.input);

    std::ofstream output_file;
    std::ostream *output = &std::cout;
    if (!command_line.output.empty()) {
      output_file.open(command_line.output);
      if (!output_file) {
        throw std::runtime_error("cannot open output file: " +
                                 command_line.output);
      }
      output = &output_file;
    }

    const auto start = std::chrono::steady_clock::now();
    if (command_line.algorithm == unary::Algorithm::Adaptive) {
      unary::AdaptiveOptions options;
      options.sparsify = command_line.sparsify;
      if (command_line.add_reverse_edges) {
        options.input_policy =
            unary::BidirectedInputPolicy::AddMissingReverseEdges;
      }
      unary::AdaptiveResult result;
      if (command_line.shallow_threshold) {
        result = unary::AdaptiveSolver{}.solveShallow(
            graph, *command_line.shallow_threshold, options);
      } else {
        result = unary::AdaptiveSolver{}.solve(graph, options);
      }
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start);
      printAdaptiveResult(*output, command_line, graph, result,
                          elapsed.count());
    } else {
      unary::FixedCounterOptions options;
      options.sparsify = command_line.sparsify;
      if (command_line.add_reverse_edges) {
        options.input_policy =
            unary::BidirectedInputPolicy::AddMissingReverseEdges;
      }
      const unary::FixedCounterResult result =
          unary::FixedCounterSolver{}.solve(graph, options);
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start);
      printFixedCounterResult(*output, command_line, graph, result,
                              elapsed.count());
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-interleaved-dyck-unary: " << error.what() << '\n';
    return 1;
  }
}
