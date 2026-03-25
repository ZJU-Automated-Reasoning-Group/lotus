#pragma once

#include <string>

namespace lotus {
namespace gvfg {

class GuardedValueFlowGraph;

class GuardedValueFlowSerializer {
public:
  static std::string toText(const GuardedValueFlowGraph &graph);
  static std::string toDot(const GuardedValueFlowGraph &graph);

  static bool writeText(const GuardedValueFlowGraph &graph,
                        const std::string &filename);
  static bool writeDot(const GuardedValueFlowGraph &graph,
                       const std::string &filename);
};

} // namespace gvfg
} // namespace lotus
