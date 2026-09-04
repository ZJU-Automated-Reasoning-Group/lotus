#include "CFL/Classical/Core/RecursiveStateMachine.h"
#include "CFL/Classical/Solvers/Preprocessing/RSMFoldability.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace lotus::cfl::classical;

namespace {

void usage(std::ostream &output) {
  output << "Usage: lotus-cfl-foldability [--all] RSM_FILE PATTERN_FILE\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    bool print_all = false;
    int first_input = 1;
    if (argc > 1 && std::string(argv[1]) == "--all") {
      print_all = true;
      ++first_input;
    }
    if (argc - first_input != 2) {
      usage(std::cerr);
      return 1;
    }

    const RecursiveStateMachine rsm =
        RecursiveStateMachine::parseFromFile(argv[first_input]);
    const FoldabilityChecker checker(rsm);
    std::ifstream patterns(argv[first_input + 1]);
    if (!patterns) {
      throw std::runtime_error("Failed to open pattern file: " +
                               std::string(argv[first_input + 1]));
    }

    std::size_t total = 0;
    std::size_t foldable = 0;
    for (std::string line; std::getline(patterns, line);) {
      if (line.empty() || line.front() == '#') {
        continue;
      }
      ++total;
      const bool accepted =
          checker.isFoldable(NodePairPattern::parse(line, rsm));
      foldable += accepted ? 1 : 0;
      if (accepted || print_all) {
        std::cout << (accepted ? "foldable\t" : "not-foldable\t") << line
                  << '\n';
      }
    }
    std::cerr << "patterns=" << total << " foldable=" << foldable << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
