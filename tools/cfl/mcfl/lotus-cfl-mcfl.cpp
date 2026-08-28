#include "CFL/MCFL/Graph.h"
#include "CFL/MCFL/InterleavedDyck.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace mcfl = lotus::cfl::mcfl;

namespace {

struct CommandLine {
  std::string input;
  std::string output;
  unsigned dimension = 2;
  mcfl::InterleavedGrammarVariant variant =
      mcfl::InterleavedGrammarVariant::Full;
  mcfl::CondensationExpansionPolicy expansion_policy =
      mcfl::CondensationExpansionPolicy::ReachabilityFiltered;
  bool condense = true;
  bool stats = false;
  bool print_pairs = false;
};

void usage(std::ostream &output) {
  output << "usage: lotus-cfl-mcfl [options] <graph.dot>\n"
            "\n"
            "Compute the paper's MCFL underapproximation of interleaved-"
            "Dyck reachability.\n"
            "\n"
            "options:\n"
            "  -d, --dimension N  run staged G_1 ... G_N (default: 2)\n"
            "  --simple           use G_d^circ instead of full G_d^+\n"
            "  --no-condense      disable the artifact's cycle elimination\n"
            "  --artifact-compatible\n"
            "                     use the artifact's condensed cross-product\n"
            "  --stats            print saturation statistics\n"
            "  --print-pairs      print final non-reflexive reachable pairs\n"
            "  -o FILE            write output to FILE\n"
            "  -h, --help         show this help\n";
}

unsigned parseUnsigned(std::string_view text, std::string_view option) {
  unsigned value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value == 0) {
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
    if (argument == "-d" || argument == "--dimension") {
      if (++i == argc) {
        throw std::invalid_argument("missing value for --dimension");
      }
      result.dimension = parseUnsigned(argv[i], argument);
      continue;
    }
    if (argument == "--simple") {
      result.variant = mcfl::InterleavedGrammarVariant::Simple;
      continue;
    }
    if (argument == "--no-condense") {
      result.condense = false;
      continue;
    }
    if (argument == "--artifact-compatible") {
      result.expansion_policy =
          mcfl::CondensationExpansionPolicy::ArtifactCompatible;
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
                 const mcfl::InterleavedAnalysisResult &result,
                 std::int64_t elapsed_ms) {
  const char *grammar_suffix =
      command_line.variant == mcfl::InterleavedGrammarVariant::Full ? "+"
                                                                    : "circ";
  for (const mcfl::InterleavedDimensionResult &dimension : result.dimensions) {
    output << "Reachable pairs (L(G^" << grammar_suffix << "_"
           << dimension.dimension << ")): " << dimension.reachable_pairs.size()
           << '\n';
    if (command_line.stats) {
      output << "  facts: " << dimension.stats.facts << '\n'
             << "  worklist pops: " << dimension.stats.worklist_pops << '\n'
             << "  type-5 combinations: " << dimension.stats.type5_combinations
             << '\n'
             << "  unreachable tuples pruned: "
             << dimension.stats.rejected_unreachable_gaps << '\n';
    }
  }
  output << "Time (ms): " << elapsed_ms << '\n';
  if (command_line.print_pairs) {
    for (const mcfl::Pair &pair : result.reachablePairs()) {
      output << pair.source << ' ' << pair.target << '\n';
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const CommandLine command_line = parseCommandLine(argc, argv);
    const mcfl::Graph graph = mcfl::Graph::parseDotFile(command_line.input);
    mcfl::InterleavedOptions options;
    options.max_dimension = command_line.dimension;
    options.variant = command_line.variant;
    options.condense = command_line.condense;
    options.expansion_policy = command_line.expansion_policy;

    const auto start = std::chrono::steady_clock::now();
    const mcfl::InterleavedAnalysisResult result =
        mcfl::InterleavedDyckSolver{}.solve(graph, options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (command_line.output.empty()) {
      printResult(std::cout, command_line, result, elapsed.count());
    } else {
      std::ofstream output(command_line.output);
      if (!output) {
        throw std::runtime_error("cannot open output file: " +
                                 command_line.output);
      }
      printResult(output, command_line, result, elapsed.count());
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lotus-cfl-mcfl: " << error.what() << '\n';
    return 1;
  }
}
