#include "CFL/Classical/SCSolver.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lotus::cfl::classical {
namespace {

std::string nodeVariable(std::size_t id) {
  std::string variable = "X";
  variable += std::to_string(id);
  return variable;
}

} // namespace

std::vector<std::string> SCSolver::splitConstraint(const std::string &value) {
  std::vector<std::string> parts;
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, ',')) {
    parts.push_back(part);
  }
  return parts;
}

SCSolver::ConstraintSystem
SCSolver::buildConstraintSystem(const LabeledGraph &graph,
                                const Grammar &grammar) const {
  ConstraintSystem system;

  for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
    const auto index = std::to_string(i);
    const auto x = nodeVariable(i);

    std::string node_constraint = x;
    node_constraint += ",node";
    node_constraint += index;
    system.con0[x].insert(std::move(node_constraint));
    system.set_variables.insert(x);
  }

  for (const auto &[label, pairs] : graph.symbolPairs()) {
    for (const auto &[source, target] : pairs) {
      const auto left = nodeVariable(source);
      const auto right = nodeVariable(target);

      std::string edge_constraint = left;
      edge_constraint += ',';
      edge_constraint += label;
      edge_constraint += ',';
      edge_constraint += right;
      system.con1[left].insert(std::move(edge_constraint));
    }
  }

  for (const auto &[left, productions] : grammar.productions()) {
    for (const auto &right : productions) {
      if (right.size() == 2) {
        for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
          const auto index = std::to_string(i);
          const auto x = nodeVariable(i);
          std::string rchd = "Rchd";
          rchd += right[0];
          rchd += '1';
          rchd += index;
          std::string dst = "Dst";
          dst += left;
          dst += index;

          std::string pro_from_x = rchd;
          pro_from_x += ',';
          pro_from_x += right[0];
          pro_from_x += "1,";
          pro_from_x += x;
          system.pro[x].insert(std::move(pro_from_x));

          std::string pro_from_rchd = dst;
          pro_from_rchd += ',';
          pro_from_rchd += right[1];
          pro_from_rchd += "1,";
          pro_from_rchd += rchd;
          system.pro[rchd].insert(std::move(pro_from_rchd));

          std::string con1_constraint = x;
          con1_constraint += ',';
          con1_constraint += left;
          con1_constraint += ',';
          con1_constraint += dst;
          system.con1[x].insert(std::move(con1_constraint));
          system.set_variables.insert(rchd);
          system.set_variables.insert(dst);
        }
      } else if (right.size() == 1) {
        for (std::size_t i = 0; i < graph.vertexCount(); ++i) {
          const auto index = std::to_string(i);
          const auto x = nodeVariable(i);
          if (right[0] == Grammar::kEpsilonSymbol) {
            std::string epsilon_constraint = x;
            epsilon_constraint += ',';
            epsilon_constraint += left;
            epsilon_constraint += ',';
            epsilon_constraint += x;
            system.con1[x].insert(std::move(epsilon_constraint));
            continue;
          }

          std::string dst = "Dst";
          dst += left;
          dst += index;

          std::string con1_constraint = x;
          con1_constraint += ',';
          con1_constraint += left;
          con1_constraint += ',';
          con1_constraint += dst;
          system.con1[x].insert(std::move(con1_constraint));

          std::string pro_constraint = dst;
          pro_constraint += ',';
          pro_constraint += right[0];
          pro_constraint += "1,";
          pro_constraint += x;
          system.pro[x].insert(std::move(pro_constraint));
          system.set_variables.insert(dst);
        }
      }
    }
  }

  return system;
}

SCStatistics SCSolver::solve(const LabeledGraph &graph,
                             const Grammar &grammar) const {
  const auto system = buildConstraintSystem(graph, grammar);

  std::vector<SCSolver::WorkItem> worklist;
  for (const auto &[_, entries] : system.con0) {
    for (const auto &entry : entries) {
      const auto parts = splitConstraint(entry);
      if (parts.size() == 2) {
        worklist.push_back({"con", parts[0], parts[1], ""});
      }
    }
  }

  std::unordered_map<std::string, bool> ground;
  for (const auto &variable : system.set_variables) {
    ground.emplace(variable, false);
  }

  SCStatistics stats;
  stats.constraint_variables = system.con1.size();
  stats.set_variables = system.set_variables.size();

  while (!worklist.empty()) {
    const auto item = worklist.back();
    worklist.pop_back();

    const auto &kind = std::get<0>(item);
    if (kind == "con" && std::get<3>(item).empty()) {
      const auto &x_var = std::get<1>(item);
      std::size_t iteration_count = 0;
      for (const auto &[_, constraints] : system.con1) {
        ++iteration_count;
        for (const auto &constraint : constraints) {
          const auto parts = splitConstraint(constraint);
          if (parts.size() != 3 || parts[0] != x_var) {
            continue;
          }
          if (!ground[parts[2]]) {
            ground[parts[2]] = true;
            worklist.push_back({"con", parts[0], parts[2], ""});
          }
        }
      }
      stats.classical_iterations += iteration_count;
      continue;
    }

    if (kind == "con") {
      const auto &x_var = std::get<1>(item);
      const auto &y_var = std::get<2>(item);
      const auto &z_var = std::get<3>(item);
      std::size_t iteration_count = 0;
      for (const auto &[_, constraints] : system.pro) {
        ++iteration_count;
        for (const auto &constraint : constraints) {
          const auto parts = splitConstraint(constraint);
          if (parts.size() != 3 || parts[0] != x_var || parts[2] != y_var) {
            continue;
          }
          if (ground[parts[2]] && !ground[z_var]) {
            ground[z_var] = true;
            worklist.push_back({"con", x_var, z_var, ""});
          }
        }
      }
      stats.classical_iterations += iteration_count;
      continue;
    }

    const auto &x_var = std::get<1>(item);
    const auto &y_var = std::get<2>(item);
    std::size_t iteration_count = 0;
    for (const auto &[_, constraints] : system.pro) {
      ++iteration_count;
      for (const auto &constraint : constraints) {
        const auto parts = splitConstraint(constraint);
        if (parts.size() != 3 || parts[0] != x_var || parts[2] != y_var) {
          continue;
        }
        if (ground[parts[2]] && !ground[y_var]) {
          ground[y_var] = true;
          worklist.push_back({"con", x_var, y_var, ""});
        }
      }
    }
    stats.classical_iterations += iteration_count;
  }

  for (const auto &[_, value] : ground) {
    if (value) {
      ++stats.grounded_variables;
    }
  }

  return stats;
}

} // namespace lotus::cfl::classical
