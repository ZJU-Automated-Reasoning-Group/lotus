#include "CFL/AdaptiveInterleavedDyck/AdaptiveInterleavedDyck.h"
#include "CFL/InterleavedDyckCore/Graph.h"

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

namespace adaptive = lotus::cfl::adaptive_interleaved_dyck;
namespace interleaved_dyck = lotus::cfl::interleaved_dyck;

namespace {

struct CommandLine {
  std::string input;
  std::string output;
  bool sparsify = true;
  bool add_reverse_edges = false;
  std::optional<std::size_t> shallow_threshold;
  bool stats = false;
  bool print_pairs = false;
};

void usage(std::ostream &output) {
  output << "usage: lotus-cfl-adaptive-interleaved-dyck [options] "
            "<graph.dot>\n"
            "\n"
            "Compute exact bidirected unary D1-interleaved-D1 "
            "reachability.\n"
            "\n"
            "options:\n"
            "  --direct           skip quotient sparsification\n"
            "  --bidirect         add missing complement reverse arcs; the\n"
            "                     result overapproximates the original graph\n"
            "  --shallow K        solve inside min(counter1,counter2) <= K\n"
            "  --stats            print quotient and arm sizes\n"
            "  --print-pairs      materialize non-reflexive component pairs\n"
            "  -o FILE            write output to FILE\n"
            "  -h, --help         show this help\n";
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

CommandLine parseCommandLine(int argc, char **argv) {
  CommandLine result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    if (argument == "-h" || argument == "--help") {
      usage(std::cout);
      std::exit(0);
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
  return result;
}

void printResult(std::ostream &output, const CommandLine &command_line,
                 const interleaved_dyck::Graph &graph,
                 const adaptive::AdaptiveInterleavedResult &result,
                 std::int64_t elapsed_ms) {
  std::unordered_set<std::size_t> components;
  for (const auto &[_, component] : result.components()) {
    components.insert(component);
  }
  output << (result.stats().overapproximates_original
                 ? "Symmetrized overapproximate"
                 : (command_line.shallow_threshold ? "Shallow exact" : "Exact"))
         << " adaptive components: " << components.size() << '\n';
  output << "Guarantee: "
         << (result.stats().overapproximates_original
                 ? "sound overapproximation of original directed graph"
                 : "exact for original bidirected graph")
         << '\n';
  if (command_line.stats) {
    const adaptive::AdaptiveInterleavedStats &stats = result.stats();
    output << "  input vertices/arcs: " << stats.input_vertices << '/'
           << stats.input_arcs << '\n'
           << "  quotient vertices/arcs: " << stats.quotient_vertices << '/'
           << stats.quotient_arcs << '\n'
           << "  threshold: " << stats.threshold << '\n'
           << "  vertical states/arcs: " << stats.vertical_control_states << '/'
           << stats.vertical_arcs << '\n'
           << "  horizontal states/arcs: " << stats.horizontal_control_states
           << '/' << stats.horizontal_arcs << '\n'
           << "  added reverse arcs: " << stats.added_reverse_arcs << '\n';
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    for (interleaved_dyck::Vertex source : graph.vertices()) {
      for (interleaved_dyck::Vertex target : graph.vertices()) {
        if (source != target && result.connected(source, target)) {
          output << source << ' ' << target << '\n';
        }
      }
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const interleaved_dyck::Graph graph =
        interleaved_dyck::Graph::parseDotFile(command_line.input);

    const auto start = std::chrono::steady_clock::now();
    adaptive::AdaptiveInterleavedResult result;
    adaptive::AdaptiveInterleavedOptions options;
    options.sparsify = command_line.sparsify;
    if (command_line.add_reverse_edges) {
      options.input_policy =
          adaptive::BidirectedInputPolicy::AddMissingReverseEdges;
    }
    if (command_line.shallow_threshold) {
      result = adaptive::AdaptiveInterleavedDyckSolver{}.solveShallow(
          graph, *command_line.shallow_threshold, options);
    } else {
      result = adaptive::AdaptiveInterleavedDyckSolver{}.solve(graph, options);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

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
    printResult(*output, command_line, graph, result, elapsed.count());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-adaptive-interleaved-dyck: " << error.what()
              << '\n';
    return 1;
  }
}
