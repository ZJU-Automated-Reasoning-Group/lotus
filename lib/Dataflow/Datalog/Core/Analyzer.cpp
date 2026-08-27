#include "Dataflow/Datalog/EngineInternal.h"

#include <algorithm>
#include <functional>
#include <set>

namespace lotus::datalog::internal {

std::vector<DependencyEdge>
collectDependencies(const std::vector<RuleIR> &rules) {
  std::vector<DependencyEdge> dependencies;
  for (const RuleIR &rule : rules) {
    for (const BodyItemIR &item : rule.body) {
      if (const auto *atom = std::get_if<AtomIR>(&item)) {
        dependencies.push_back(
            {atom->relation, rule.head.relation, DependencyKind::Positive});
      } else if (const auto *negation = std::get_if<NegAtomIR>(&item)) {
        dependencies.push_back({negation->atom.relation, rule.head.relation,
                                DependencyKind::Negative});
      } else if (const auto *aggregate = std::get_if<AggregateIR>(&item)) {
        if (aggregate->source_body.empty()) {
          dependencies.push_back(
              {aggregate->source.relation, rule.head.relation,
               aggregate->monotone ? DependencyKind::Positive
                                   : DependencyKind::Aggregate});
        } else {
          for (const AggregateSourceItemIR &source_item :
               aggregate->source_body) {
            if (const auto *atom = std::get_if<AtomIR>(&source_item)) {
              dependencies.push_back({atom->relation, rule.head.relation,
                                      aggregate->monotone
                                          ? DependencyKind::Positive
                                          : DependencyKind::Aggregate});
            } else if (const auto *negation =
                           std::get_if<NegAtomIR>(&source_item)) {
              dependencies.push_back({negation->atom.relation,
                                      rule.head.relation,
                                      DependencyKind::Aggregate});
            }
          }
        }
      }
    }
  }
  return dependencies;
}

std::vector<std::size_t>
computeStrata(const std::vector<DependencyEdge> &dependencies,
              std::size_t relation_count) {
  std::vector<std::size_t> strata(relation_count, 0);
  for (std::size_t iteration = 0; iteration < relation_count; ++iteration) {
    bool changed = false;
    for (const DependencyEdge &dependency : dependencies) {
      const std::size_t weight =
          dependency.kind == DependencyKind::Positive ? 0 : 1;
      const std::size_t required = strata[dependency.source] + weight;
      if (strata[dependency.target] < required) {
        strata[dependency.target] = required;
        changed = true;
        if (iteration + 1 == relation_count) {
          const char *kind = dependency.kind == DependencyKind::Negative
                                 ? "negative"
                                 : "aggregate";
          throw CompileError(std::string("program is not stratifiable: ") +
                             kind + " dependency participates in a cycle");
        }
      }
    }
    if (!changed)
      break;
  }
  return strata;
}

std::vector<PlannedSCC>
buildSCCPlan(const std::vector<RuleIR> &rules,
             const std::vector<DependencyEdge> &dependencies,
             const std::vector<std::size_t> &strata,
             std::size_t relation_count) {
  std::vector<std::vector<RelationId>> graph(relation_count);
  std::vector<bool> self_edge(relation_count, false);
  for (const DependencyEdge &dependency : dependencies) {
    if (dependency.kind != DependencyKind::Positive)
      continue;
    graph[dependency.source].push_back(dependency.target);
    if (dependency.source == dependency.target)
      self_edge[dependency.source] = true;
  }

  std::vector<int> index(relation_count, -1);
  std::vector<int> low_link(relation_count, -1);
  std::vector<bool> on_stack(relation_count, false);
  std::vector<RelationId> stack;
  std::vector<std::vector<RelationId>> components;
  int next_index = 0;

  std::function<void(RelationId)> visit = [&](RelationId node) {
    index[node] = low_link[node] = next_index++;
    stack.push_back(node);
    on_stack[node] = true;

    for (RelationId successor : graph[node]) {
      if (index[successor] == -1) {
        visit(successor);
        low_link[node] = std::min(low_link[node], low_link[successor]);
      } else if (on_stack[successor]) {
        low_link[node] = std::min(low_link[node], index[successor]);
      }
    }

    if (low_link[node] != index[node])
      return;
    components.emplace_back();
    while (true) {
      RelationId member = stack.back();
      stack.pop_back();
      on_stack[member] = false;
      components.back().push_back(member);
      if (member == node)
        break;
    }
  };

  for (RelationId relation = 0; relation < relation_count; ++relation) {
    if (index[relation] == -1)
      visit(relation);
  }

  std::vector<std::size_t> component_of(relation_count);
  for (std::size_t component = 0; component < components.size(); ++component) {
    for (RelationId relation : components[component])
      component_of[relation] = component;
  }

  std::vector<std::unordered_set<std::size_t>> component_edges(
      components.size());
  std::vector<std::size_t> indegree(components.size(), 0);
  for (RelationId source = 0; source < relation_count; ++source) {
    for (RelationId target : graph[source]) {
      std::size_t source_component = component_of[source];
      std::size_t target_component = component_of[target];
      if (source_component == target_component)
        continue;
      if (component_edges[source_component].insert(target_component).second)
        ++indegree[target_component];
    }
  }

  std::set<std::pair<std::size_t, std::size_t>> ready;
  for (std::size_t component = 0; component < components.size(); ++component) {
    if (indegree[component] == 0)
      ready.emplace(strata[components[component].front()], component);
  }

  std::vector<PlannedSCC> result;
  while (!ready.empty()) {
    std::size_t component = ready.begin()->second;
    ready.erase(ready.begin());

    PlannedSCC planned;
    planned.relations = components[component];
    planned.relation_set.insert(planned.relations.begin(),
                                planned.relations.end());
    planned.recursive = planned.relations.size() > 1;
    for (RelationId relation : planned.relations)
      planned.recursive = planned.recursive || self_edge[relation];
    planned.stratum = strata[planned.relations.front()];
    for (std::size_t rule_index = 0; rule_index < rules.size(); ++rule_index) {
      if (planned.relation_set.count(rules[rule_index].head.relation))
        planned.rules.push_back(rule_index);
    }
    result.push_back(std::move(planned));

    for (std::size_t successor : component_edges[component]) {
      if (--indegree[successor] == 0)
        ready.emplace(strata[components[successor].front()], successor);
    }
  }

  if (result.size() != components.size())
    throw CompileError("internal error while ordering Datalog SCCs");
  return result;
}

} // namespace lotus::datalog::internal
