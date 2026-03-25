#include "Checker/AE/ICFGWTO.h"

#include <algorithm>
#include <functional>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/CFG.h>

namespace lotus {
namespace analysis {

namespace {

using BB = llvm::BasicBlock;

static bool
hasSelfLoop(const BB *bb,
            const std::unordered_set<const BB *> *allowed = nullptr) {
  for (const BB *succ : llvm::successors(bb)) {
    if (succ != bb)
      continue;
    if (!allowed || allowed->count(succ) > 0)
      return true;
  }
  return false;
}

static std::vector<const BB *> computeReachableRPO(const BB *entry) {
  std::vector<const BB *> postOrder;
  std::unordered_set<const BB *> visited;

  std::function<void(const BB *)> dfs = [&](const BB *bb) {
    if (!bb || !visited.insert(bb).second)
      return;
    for (const BB *succ : llvm::successors(bb)) {
      dfs(succ);
    }
    postOrder.push_back(bb);
  };

  dfs(entry);
  std::reverse(postOrder.begin(), postOrder.end());
  return postOrder;
}

static std::vector<std::vector<const BB *>> computeSCCs(
    const std::vector<const BB *> &nodes,
    const std::unordered_map<const BB *, std::vector<const BB *>> &succAll) {
  std::unordered_set<const BB *> allowed(nodes.begin(), nodes.end());
  std::unordered_map<const BB *, int> index;
  std::unordered_map<const BB *, int> lowlink;
  std::unordered_set<const BB *> onStack;
  std::vector<const BB *> stack;
  std::vector<std::vector<const BB *>> sccs;
  int nextIndex = 0;

  std::function<void(const BB *)> strongConnect = [&](const BB *v) {
    index[v] = nextIndex;
    lowlink[v] = nextIndex;
    ++nextIndex;
    stack.push_back(v);
    onStack.insert(v);

    auto succIt = succAll.find(v);
    if (succIt != succAll.end()) {
      for (const BB *w : succIt->second) {
        if (allowed.count(w) == 0)
          continue;
        if (index.count(w) == 0) {
          strongConnect(w);
          lowlink[v] = std::min(lowlink[v], lowlink[w]);
        } else if (onStack.count(w) != 0) {
          lowlink[v] = std::min(lowlink[v], index[w]);
        }
      }
    }

    if (lowlink[v] == index[v]) {
      std::vector<const BB *> comp;
      while (!stack.empty()) {
        const BB *w = stack.back();
        stack.pop_back();
        onStack.erase(w);
        comp.push_back(w);
        if (w == v)
          break;
      }
      sccs.push_back(std::move(comp));
    }
  };

  for (const BB *bb : nodes) {
    if (index.count(bb) == 0) {
      strongConnect(bb);
    }
  }

  // Tarjan naturally emits reverse topological SCC order.
  std::reverse(sccs.begin(), sccs.end());
  return sccs;
}

} // namespace

std::vector<const llvm::BasicBlock *> ICFGSingletonWTO::getSuccessors() const {
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(bb), et = llvm::succ_end(bb); it != et;
       ++it) {
    succs.push_back(*it);
  }
  return succs;
}

std::vector<const llvm::BasicBlock *> ICFGCycleWTO::getSuccessors() const {
  std::vector<const llvm::BasicBlock *> succs;
  for (const auto *comp : components) {
    auto compSuccs = comp->getSuccessors();
    succs.insert(succs.end(), compSuccs.begin(), compSuccs.end());
  }
  return succs;
}

std::vector<const llvm::BasicBlock *>
ICFGCycleWTO::getExitSuccessors(const llvm::BasicBlock *exitBB) const {
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(exitBB), et = llvm::succ_end(exitBB);
       it != et; ++it) {
    const llvm::BasicBlock *succ = *it;
    bool inside = false;
    for (const auto *comp : components) {
      if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
        if (singleton->getBlock() == succ) {
          inside = true;
          break;
        }
      }
    }
    if (!inside) {
      succs.push_back(succ);
    }
  }
  return succs;
}

ICFGWTO::ICFGWTO(const llvm::Function *f) : func(f), entry(nullptr) {
  if (!f->empty()) {
    entry = &f->getEntryBlock();
    buildWTO();
  }
}

void ICFGWTO::buildWTO() {
  if (!func || !entry)
    return;

  std::unordered_map<const BB *, std::vector<const BB *>> succAll;
  for (const BB &bb : *func) {
    auto &succs = succAll[&bb];
    for (const BB *succ : llvm::successors(&bb)) {
      succs.push_back(succ);
    }
  }

  std::vector<const BB *> rpo = computeReachableRPO(entry);
  std::unordered_map<const BB *, size_t> rpoIndex;
  for (size_t i = 0; i < rpo.size(); ++i) {
    rpoIndex[rpo[i]] = i;
  }

  std::unordered_set<const BB *> reachable(rpo.begin(), rpo.end());
  std::set<const BB *> represented;

  std::function<const ICFGWTOComp *(const std::vector<const BB *> &,
                                    const BB *)>
      buildFromNodes;
  buildFromNodes = [&](const std::vector<const BB *> &nodes,
                       const BB *preferredHead) -> const ICFGWTOComp * {
    if (nodes.empty())
      return nullptr;

    std::unordered_set<const BB *> nodeSet(nodes.begin(), nodes.end());
    bool cyclic = (nodes.size() > 1);
    if (!cyclic) {
      cyclic = hasSelfLoop(nodes.front(), &nodeSet);
    }

    if (!cyclic) {
      represented.insert(nodes.front());
      return new ICFGSingletonWTO(nodes.front());
    }

    const BB *head = nullptr;
    if (preferredHead && nodeSet.count(preferredHead) > 0) {
      head = preferredHead;
    } else {
      size_t best = std::numeric_limits<size_t>::max();
      for (const BB *bb : nodes) {
        auto it = rpoIndex.find(bb);
        size_t idx = (it == rpoIndex.end()) ? best : it->second;
        if (!head || idx < best) {
          head = bb;
          best = idx;
        }
      }
      if (!head) {
        head = nodes.front();
      }
    }

    represented.insert(head);
    auto *cycle = new ICFGCycleWTO(head);

    std::vector<const BB *> remaining;
    remaining.reserve(nodes.size());
    for (const BB *bb : nodes) {
      if (bb != head)
        remaining.push_back(bb);
    }
    if (remaining.empty())
      return cycle;

    for (const auto &subScc : computeSCCs(remaining, succAll)) {
      for (const BB *bb : subScc) {
        represented.insert(bb);
      }
      if (const ICFGWTOComp *subComp = buildFromNodes(subScc, nullptr)) {
        cycle->addComponent(subComp);
      }
    }

    return cycle;
  };

  for (const auto &topScc : computeSCCs(rpo, succAll)) {
    const BB *preferred =
        (std::find(topScc.begin(), topScc.end(), entry) != topScc.end())
            ? entry
            : nullptr;
    if (const ICFGWTOComp *comp = buildFromNodes(topScc, preferred)) {
      components.push_back(comp);
    }
  }

  // Preserve total coverage, including unreachable blocks.
  for (const BB &bb : *func) {
    if (reachable.count(&bb) == 0 || represented.count(&bb) == 0) {
      components.push_back(new ICFGSingletonWTO(&bb));
    }
  }
}

const ICFGWTOComp *
ICFGWTO::buildComponent(const llvm::BasicBlock *bb,
                        std::set<const llvm::BasicBlock *> &visited,
                        std::set<const llvm::BasicBlock *> &inStack) {
  if (!bb)
    return nullptr;

  if (visited.count(bb))
    return nullptr;

  visited.insert(bb);
  inStack.insert(bb);

  bool hasBackEdge = false;
  std::vector<const llvm::BasicBlock *> succs;
  for (auto it = llvm::succ_begin(bb), et = llvm::succ_end(bb); it != et;
       ++it) {
    const llvm::BasicBlock *succ = *it;
    succs.push_back(succ);
    if (inStack.count(succ)) {
      hasBackEdge = true;
    }
  }

  if (!hasBackEdge) {
    inStack.erase(bb);
    return new ICFGSingletonWTO(bb);
  }

  auto *cycle = new ICFGCycleWTO(bb);

  for (const llvm::BasicBlock *succ : succs) {
    if (inStack.count(succ)) {
      cycle->addComponent(new ICFGSingletonWTO(succ));
    } else if (!visited.count(succ)) {
      const ICFGWTOComp *subComp = buildComponent(succ, visited, inStack);
      if (subComp) {
        cycle->addComponent(subComp);
      }
    }
  }

  inStack.erase(bb);
  return cycle;
}

std::vector<const llvm::BasicBlock *>
ICFGWTO::getSuccessors(const llvm::BasicBlock *bb) const {
  for (const auto *comp : components) {
    if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
      if (singleton->getBlock() == bb) {
        return singleton->getSuccessors();
      }
    }
    if (const auto *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
      for (const auto *innerComp : cycle->getComponents()) {
        if (const auto *innerSingleton =
                llvm::dyn_cast<ICFGSingletonWTO>(innerComp)) {
          if (innerSingleton->getBlock() == bb) {
            return innerSingleton->getSuccessors();
          }
        }
      }
    }
  }
  return {};
}

std::vector<const llvm::BasicBlock *> ICFGWTO::getNodes() const {
  std::vector<const llvm::BasicBlock *> nodes;
  std::set<const llvm::BasicBlock *> visited;

  std::function<void(const ICFGWTOComp *)> collect =
      [&](const ICFGWTOComp *comp) {
        if (const auto *singleton = llvm::dyn_cast<ICFGSingletonWTO>(comp)) {
          if (visited.insert(singleton->getBlock()).second) {
            nodes.push_back(singleton->getBlock());
          }
        } else if (const auto *cycle = llvm::dyn_cast<ICFGCycleWTO>(comp)) {
          for (const auto *innerComp : cycle->getComponents()) {
            collect(innerComp);
          }
        }
      };

  for (const auto *comp : components) {
    collect(comp);
  }

  return nodes;
}

} // namespace analysis
} // namespace lotus
