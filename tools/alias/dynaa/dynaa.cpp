#include "Alias/Dynamic/DynamicAliasAnalysis.h"

#include <iostream>

int main(int argc, char **argv) {
  // Unsync iostream with C I/O libraries to accelerate standard iostreams
  std::ios::sync_with_stdio(false);

  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <input log filename>\n\n";
    std::exit(-1);
  }

  dynamic::DynamicAliasAnalysis dynAA(argv[1]);
  dynAA.runAnalysis();

  for (auto const &mapping : dynAA) {
    if (mapping.second.empty())
      continue;

    std::cout << "Function# " << mapping.first << ":\n";
    for (auto const &pair : mapping.second) {
      std::cout << "  Ptr# " << pair.getFirst() << ", Ptr# " << pair.getSecond()
                << '\n';
    }
  }
}