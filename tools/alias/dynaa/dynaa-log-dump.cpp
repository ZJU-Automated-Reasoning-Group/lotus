#include "Alias/Dynamic/LogPrinter.h"

#include <iostream>

int main(int argc, char **argv) {
  // Unsync iostream with C I/O libraries to accelerate standard iostreams
  std::ios::sync_with_stdio(false);

  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <input log filename>\n\n";
    std::exit(-1);
  }

  dynamic::LogPrinter(argv[1], std::cout).process();
}