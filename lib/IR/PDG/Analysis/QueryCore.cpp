#include "IR/PDG/Analysis/QueryCore.h"

#include "IR/PDG/Analysis/Internal/QuerySupport.h"
#include "IR/PDG/Analysis/Query.h"
#include "IR/PDG/QueryLanguage/Cypher.h"

#include <sstream>

namespace pdg {
using namespace llvm;
using namespace query_detail;

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset) {
  switch (preset) {
  case PDGEdgePreset::All:
    return {EdgeType::IND_CALL,
            EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY,
            EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR,
            EdgeType::DATA_DEF_USE,
            EdgeType::DATA_RAW,
            EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,
            EdgeType::VAL_DEP,
            EdgeType::CLS_MTH,
            EdgeType::ANNO_VAR,
            EdgeType::ANNO_GLOBAL,
            EdgeType::ANNO_OTHER,
            EdgeType::TYPE_OTHEREDGE};
  case PDGEdgePreset::Data:
    return {EdgeType::DATA_DEF_USE,  EdgeType::DATA_RAW,
            EdgeType::DATA_READ,     EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,      EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT, EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,    EdgeType::VAL_DEP};
  case PDGEdgePreset::Control:
    return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR};
  case PDGEdgePreset::Parameter:
    return {EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::Interprocedural:
    return {EdgeType::IND_CALL,           EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET, EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,       EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::ValueFlow:
    return {EdgeType::DATA_DEF_USE,    EdgeType::DATA_RAW,
            EdgeType::DATA_READ,       EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,        EdgeType::VAL_DEP,
            EdgeType::PARAMETER_IN,    EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD, EdgeType::GLOBAL_DEP};
  case PDGEdgePreset::TransformLegality:
    return {EdgeType::DATA_DEF_USE,
            EdgeType::DATA_RAW,
            EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD,
            EdgeType::VAL_DEP,
            EdgeType::GLOBAL_DEP,
            EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY,
            EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR};
  }
  return std::set<EdgeType>();
}

std::string describeNode(Node *node) {
  if (node == nullptr)
    return "<null>";
  std::string description = pdgutils::getNodeTypeStr(node->getNodeType());
  if (node->getValue() != nullptr) {
    description += " ";
    description += stringifyValue(node->getValue());
  }
  return description;
}

std::string stableNodeKey(Node *node) {
  if (node == nullptr)
    return "<null>";

  std::ostringstream os;
  os << pdgutils::getNodeTypeStr(node->getNodeType()) << "|"
     << functionNameForNode(node) << "|";
  const std::string source = sourceKeyForNode(node);
  if (!source.empty())
    os << source;
  else if (node->getValue() != nullptr)
    os << stringifyValue(node->getValue());
  else
    os << pointerKey(node);
  return os.str();
}

std::string resourceKindName(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::Heap:
    return "heap";
  case ResourceKind::File:
    return "file";
  case ResourceKind::FileDescriptor:
    return "fd";
  case ResourceKind::Directory:
    return "dir";
  case ResourceKind::Lock:
    return "lock";
  case ResourceKind::Unknown:
  default:
    return "unknown";
  }
}

PDGQueryResult PDGCriteriaResolver::resolve(const PDGCriteria &criteria,
                                            const PDGQueryOptions &options,
                                            const Module *module) const {
  PDGQueryResult result;

  for (NodeSet::const_iterator it = criteria.nodes.begin();
       it != criteria.nodes.end(); ++it) {
    if (*it != nullptr)
      result.nodes.insert(*it);
  }

  for (size_t i = 0; i < criteria.values.size(); ++i) {
    Value *value = criteria.values[i];
    if (value == nullptr)
      continue;
    if (Node *node = pdg_.getNode(*value))
      result.nodes.insert(node);
    else
      result.diagnostics.unresolved_criteria.push_back("value has no PDG node");
  }

  if (!criteria.function_names.empty()) {
    for (ProgramGraph::FuncWrapperMap::iterator it =
             pdg_.getFuncWrapperMap().begin();
         it != pdg_.getFuncWrapperMap().end(); ++it) {
      Function *function = it->first;
      if (function == nullptr)
        continue;
      const std::string function_name = toLower(function->getName().str());
      for (size_t name_index = 0; name_index < criteria.function_names.size();
           ++name_index) {
        if (function_name == toLower(criteria.function_names[name_index])) {
          if (Node *entry = pdg_.getNode(*function))
            result.nodes.insert(entry);
        }
      }
    }
  }

  if (!criteria.callee_names.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "callee criteria requires module");
    } else {
      for (const Function &function : *module) {
        if (function.isDeclaration())
          continue;
        for (const BasicBlock &block : function) {
          for (const Instruction &inst : block) {
            const CallBase *call = dyn_cast<CallBase>(&inst);
            if (call == nullptr || call->getCalledFunction() == nullptr)
              continue;
            const std::string callee =
                toLower(call->getCalledFunction()->getName().str());
            for (size_t i = 0; i < criteria.callee_names.size(); ++i) {
              if (callee == toLower(criteria.callee_names[i])) {
                if (Node *node = pdg_.getNode(const_cast<Instruction &>(inst)))
                  result.nodes.insert(node);
              }
            }
          }
        }
      }
    }
  }

  if (!criteria.source_locations.empty()) {
    for (ProgramGraph::NodeSet::iterator it = pdg_.begin(); it != pdg_.end();
         ++it) {
      Node *node = *it;
      const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
      if (inst == nullptr)
        continue;
      for (size_t location_index = 0;
           location_index < criteria.source_locations.size();
           ++location_index) {
        if (sourceLocationMatches(criteria.source_locations[location_index],
                                  *inst)) {
          result.nodes.insert(node);
          break;
        }
      }
    }
  }

  if (!criteria.property_specs.empty()) {
    if (module == nullptr) {
      result.diagnostics.unresolved_criteria.push_back(
          "property criteria requires module");
    } else {
      for (size_t i = 0; i < criteria.property_specs.size(); ++i) {
        NodeSet property_nodes =
            resolvePropertyCriteria(pdg_, *module, criteria.property_specs[i]);
        result.nodes.insert(property_nodes.begin(), property_nodes.end());
      }
    }
  }

  for (size_t i = 0; i < criteria.cypher_selections.size(); ++i) {
    const CypherSelection &selection = criteria.cypher_selections[i];
    CypherParser parser;
    std::unique_ptr<CypherQuery> query = parser.parse(selection.query);
    if (!query) {
      result.diagnostics.unresolved_criteria.push_back(
          parser.getLastError().message);
      continue;
    }

    CypherQueryExecutor executor(pdg_);
    std::unique_ptr<CypherResult> cypher_result = executor.execute(*query);
    if (!cypher_result) {
      result.diagnostics.unresolved_criteria.push_back(executor.getLastError());
      continue;
    }

    if (!selection.binding.empty()) {
      const std::vector<Node *> *bound =
          executor.getBoundVariable(selection.binding);
      if (bound == nullptr) {
        result.diagnostics.unresolved_criteria.push_back(
            "missing Cypher binding: " + selection.binding);
        continue;
      }
      result.nodes.insert(bound->begin(), bound->end());
    } else {
      result.nodes.insert(cypher_result->getNodes().begin(),
                          cypher_result->getNodes().end());
    }
  }

  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  if (!scoped_nodes.empty()) {
    NodeSet filtered;
    for (NodeSet::const_iterator it = result.nodes.begin();
         it != result.nodes.end(); ++it) {
      if (scoped_nodes.count(*it) != 0)
        filtered.insert(*it);
    }
    if (filtered.empty() && !result.nodes.empty())
      result.diagnostics.unresolved_criteria.push_back(
          "all resolved criteria were excluded by scope");
    result.nodes.swap(filtered);
  }

  result.criteria_nodes = result.nodes;
  return result;
}

} // namespace pdg
