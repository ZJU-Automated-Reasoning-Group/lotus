#include "CFL/Classical/Clients/ValueFlow/SVFGPreparation.h"

#include "IR/ICFG/CallGraph.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace lotus::cfl::classical {
namespace {

using RecursiveFunctionSet = std::unordered_set<const llvm::Function *>;

RecursiveFunctionSet
findRecursiveFunctionsInRefinedCallGraph(const LTCallGraph &call_graph) {
  std::unordered_map<const llvm::Function *, int> index;
  std::unordered_map<const llvm::Function *, int> lowlink;
  std::unordered_set<const llvm::Function *> on_stack;
  std::vector<const llvm::Function *> stack;
  RecursiveFunctionSet recursive;
  int next_index = 0;

  std::function<void(const llvm::Function *)> visit =
      [&](const llvm::Function *function) {
        index[function] = lowlink[function] = next_index++;
        stack.push_back(function);
        on_stack.insert(function);

        const LTCallGraphNode *node = call_graph[function];
        for (const auto &edge : *node) {
          const LTCallGraphNode *callee_node = edge.second;
          const llvm::Function *callee =
              callee_node ? callee_node->getFunction() : nullptr;
          if (!callee || callee->isDeclaration()) {
            continue;
          }
          if (index.count(callee) == 0) {
            visit(callee);
            lowlink[function] = std::min(lowlink[function], lowlink[callee]);
          } else if (on_stack.count(callee) != 0) {
            lowlink[function] = std::min(lowlink[function], index[callee]);
          }
        }

        if (lowlink[function] != index[function]) {
          return;
        }
        std::vector<const llvm::Function *> component;
        while (!stack.empty()) {
          const llvm::Function *member = stack.back();
          stack.pop_back();
          on_stack.erase(member);
          component.push_back(member);
          if (member == function) {
            break;
          }
        }

        bool is_recursive = component.size() > 1;
        if (!is_recursive && !component.empty()) {
          const LTCallGraphNode *single = call_graph[component.front()];
          for (const auto &edge : *single) {
            is_recursive = is_recursive || edge.second == single;
          }
        }
        if (is_recursive) {
          recursive.insert(component.begin(), component.end());
        }
      };

  for (const auto &entry : call_graph) {
    const llvm::Function *function = entry.first;
    if (function && !function->isDeclaration() && index.count(function) == 0) {
      visit(function);
    }
  }
  return recursive;
}

const llvm::Module *findModule(const lotus::analysis::SVFG &svfg) {
  if (const LTCallGraph *call_graph = svfg.getRefinedCallGraph()) {
    return &call_graph->getModule();
  }
  for (const auto &[_, node] : svfg) {
    if (node) {
      if (const llvm::Function *function = node->getFunction()) {
        return function->getParent();
      }
      if (const llvm::Instruction *instruction = node->getInstruction()) {
        return instruction->getModule();
      }
    }
  }
  return nullptr;
}

RecursiveFunctionSet findRecursiveFunctions(const lotus::analysis::SVFG &svfg) {
  if (const LTCallGraph *call_graph = svfg.getRefinedCallGraph()) {
    return findRecursiveFunctionsInRefinedCallGraph(*call_graph);
  }

  const llvm::Module *module = findModule(svfg);
  if (!module) {
    return {};
  }
  llvm::CallGraph call_graph(*const_cast<llvm::Module *>(module));
  RecursiveFunctionSet recursive;
  for (auto component = llvm::scc_begin(&call_graph),
            end = llvm::scc_end(&call_graph);
       component != end; ++component) {
    const std::vector<llvm::CallGraphNode *> &nodes = *component;
    bool is_recursive = nodes.size() > 1;
    if (!is_recursive && nodes.size() == 1) {
      llvm::CallGraphNode *node = nodes.front();
      for (const auto &edge : *node) {
        is_recursive = is_recursive || edge.second == node;
      }
    }
    if (!is_recursive) {
      continue;
    }
    for (llvm::CallGraphNode *node : nodes) {
      if (node && node->getFunction()) {
        recursive.insert(node->getFunction());
      }
    }
  }
  return recursive;
}

const llvm::Function *owningFunction(const llvm::Value *value) {
  if (!value) {
    return nullptr;
  }
  if (const auto *instruction = llvm::dyn_cast<llvm::Instruction>(value)) {
    return instruction->getFunction();
  }
  if (const auto *argument = llvm::dyn_cast<llvm::Argument>(value)) {
    return argument->getParent();
  }
  if (const auto *expression = llvm::dyn_cast<llvm::ConstantExpr>(value)) {
    if (expression->isCast() ||
        expression->getOpcode() == llvm::Instruction::GetElementPtr) {
      return owningFunction(expression->getOperand(0));
    }
  }
  return nullptr;
}

bool isStrongUpdateStore(const lotus::analysis::SVFG &svfg,
                         const lotus::analysis::StoreSVFGNode &store,
                         const RecursiveFunctionSet &recursive_functions) {
  const auto &points_to = store.getMemoryPointsTo();
  if (points_to.size() != 1) {
    return false;
  }
  const std::uint32_t object = *points_to.begin();
  const auto *info = svfg.getObjectInfo(object);
  if (!info || info->isHeap || info->isArray || info->isFieldInsensitive ||
      info->isUnknown) {
    return false;
  }
  if (!info->isStack) {
    return true;
  }

  const llvm::Value *object_value = svfg.getObjectValue(object);
  if (!object_value && info->baseObjId != 0) {
    object_value = svfg.getObjectValue(info->baseObjId);
  }
  const llvm::Function *function = owningFunction(object_value);
  return function && recursive_functions.count(function) == 0;
}

bool isDereferenceInput(const lotus::analysis::SVFGEdge &edge,
                        const lotus::analysis::SVFGNode &node) {
  if (const auto *store =
          llvm::dyn_cast<lotus::analysis::StoreSVFGNode>(&node)) {
    return edge.getSrcNode()->getId() == store->getStoreToPtr() &&
           (edge.isStoreEdge() || edge.isDirectEdge());
  }
  if (const auto *load = llvm::dyn_cast<lotus::analysis::LoadSVFGNode>(&node)) {
    return edge.getSrcNode()->getId() == load->getLoadFromPtr() &&
           (edge.isLoadEdge() || edge.isDirectEdge());
  }
  return false;
}

} // namespace

SVFGPreparationStatistics
prepareSVFGForCFL(lotus::analysis::SVFG &svfg,
                  const SVFGPreparationOptions &options) {
  SVFGPreparationStatistics statistics;
  const RecursiveFunctionSet recursive_functions =
      options.prune_strong_update_inputs ? findRecursiveFunctions(svfg)
                                         : RecursiveFunctionSet{};
  for (const auto &[_, node] : svfg) {
    if (!node) {
      continue;
    }
    const auto *store = llvm::dyn_cast<lotus::analysis::StoreSVFGNode>(node);
    const bool strong_update =
        store && options.prune_strong_update_inputs &&
        isStrongUpdateStore(svfg, *store, recursive_functions);
    if (store) {
      ++statistics.stores_examined;
      statistics.strong_update_stores += strong_update ? 1 : 0;
    }

    std::vector<lotus::analysis::SVFGEdge *> remove;
    for (lotus::analysis::SVFGEdge *edge : node->getInEdges()) {
      if (!edge) {
        continue;
      }
      if (options.remove_dereference_direct_edges &&
          isDereferenceInput(*edge, *node)) {
        remove.push_back(edge);
        ++statistics.dereference_edges_removed;
      } else if (strong_update && edge->isIndirectEdge()) {
        remove.push_back(edge);
        ++statistics.strong_update_edges_removed;
      }
    }
    for (lotus::analysis::SVFGEdge *edge : remove) {
      svfg.removeEdge(edge);
    }
  }
  return statistics;
}

} // namespace lotus::cfl::classical
