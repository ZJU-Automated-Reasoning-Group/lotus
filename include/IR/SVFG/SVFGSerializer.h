#pragma once

#include <string>

namespace lotus {
namespace analysis {

class SVFG;

class SVFGSerializer {
public:
  static bool writeDot(const SVFG &graph, const std::string &filename);
  static bool writeText(const SVFG &graph, const std::string &filename);
  static bool readText(SVFG &graph, const std::string &filename);
};

} // namespace analysis
} // namespace lotus
