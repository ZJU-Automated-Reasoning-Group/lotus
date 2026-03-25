/**
 * @file FragmentDecomposition.cpp
 * @brief Algorithms for selecting abstraction points and constructing acyclic
 *        `Fragment`s between them for use by the SymAbsAI analyzer.

 * Author: rainoftime
 */
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"

#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Config.h"

#include <algorithm>
#include <map>
#include <queue>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>

namespace symabs_ai {
namespace {
std::vector<Fragment::edge> out_edges(llvm::BasicBlock *bb) {
  std::vector<Fragment::edge> result;

  if (bb == Fragment::EXIT)
    return result;

  auto itr = llvm::succ_begin(bb), end = llvm::succ_end(bb);
  if (itr == end) {
    result.push_back({bb, Fragment::EXIT});
  } else {
    for (; itr != end; ++itr)
      result.push_back({bb, *itr});
  }

  return result;
}

std::vector<Fragment::edge> in_edges(llvm::BasicBlock *bb,
                                     llvm::Function *func) {
  std::vector<Fragment::edge> result;

  if (bb == Fragment::EXIT) {
    for (auto &bb_prev : *func) {
      if (llvm::succ_begin(&bb_prev) == llvm::succ_end(&bb_prev))
        result.push_back({&bb_prev, bb});
    }
    return result;
  }

  for (auto itr = llvm::pred_begin(bb); itr != llvm::pred_end(bb); ++itr) {
    result.push_back({*itr, bb});
  }

  return result;
}

std::set<Fragment::edge> find_reachable(
    llvm::BasicBlock *start, llvm::Function *func, llvm::BasicBlock *ending,
    const std::set<llvm::BasicBlock *> &forbidden, bool backwards = false) {
  std::set<Fragment::edge> visited;
  std::queue<llvm::BasicBlock *> queue;
  queue.push(start);

  while (!queue.empty()) {
    llvm::BasicBlock *bb_prev = queue.front();
    queue.pop();

    auto edges = backwards ? in_edges(bb_prev, func) : out_edges(bb_prev);
    for (auto &edge : edges) {
      llvm::BasicBlock *bb_next = backwards ? edge.first : edge.second;

      bool allowed = forbidden.find(bb_next) == forbidden.end();
      bool edge_new = visited.find(edge) == visited.end();

      if (allowed && edge_new) {
        visited.insert(edge);
        if (bb_next != ending)
          queue.push(bb_next);
      }
    }
  }

  return visited;
}

std::vector<Fragment::edge>
find_edges(llvm::BasicBlock *start, llvm::BasicBlock *end, llvm::Function *func,
           const std::set<llvm::BasicBlock *> &abstraction_points) {
  std::set<llvm::BasicBlock *> forbidden(abstraction_points.begin(),
                                         abstraction_points.end());

  // all edges reachable from `start' without passing through abstraction
  // points (apart from `end')
  forbidden.insert(start);
  forbidden.erase(end);
  std::set<Fragment::edge> forward =
      find_reachable(start, func, end, forbidden, false);

  // all edges reachable from `end' going backwards and without any
  // abstraction points on the way (apart from `start')
  forbidden.insert(end);
  forbidden.erase(start);
  std::set<Fragment::edge> backward =
      find_reachable(end, func, start, forbidden, true);

  std::vector<Fragment::edge> result;
  std::set_intersection(forward.begin(), forward.end(), backward.begin(),
                        backward.end(), std::back_inserter(result));

  return result;
}

void dfs(llvm::BasicBlock *bb, std::map<llvm::BasicBlock *, int> *dfn, int *c) {
  assert(dfn->find(bb) == dfn->end());
  dfn->insert({bb, 0});

  for (auto itr = llvm::succ_begin(bb); itr != llvm::succ_end(bb); ++itr) {
    if (dfn->find(*itr) == dfn->end())
      dfs(*itr, dfn, c);
  }

  (*dfn)[bb] = *c;
  *c = *c - 1;
}

std::vector<Fragment::edge> find_retreating(llvm::Function *func) {
  std::vector<Fragment::edge> result;
  std::map<llvm::BasicBlock *, int> dfn;
  int c = func->size();

  dfs(&func->getEntryBlock(), &dfn, &c);

  for (llvm::BasicBlock &bb : *func) {
    for (auto itr = llvm::succ_begin(&bb); itr != succ_end(&bb); ++itr) {
      bool is_retr = false;

      if (*itr == &bb) {
        is_retr = true;
      } else {
        auto kv_pre = dfn.find(&bb);
        auto kv_post = dfn.find(*itr);

        if (kv_pre != dfn.end() && kv_post != dfn.end())
          is_retr = (kv_pre->second > kv_post->second);
      }

      if (is_retr)
        result.push_back({&bb, *itr});
    }
  }

  return result;
}
} // namespace

std::set<llvm::BasicBlock *>
FragmentDecomposition::GetAbstractionPoints(llvm::Function *func,
                                            strategy strat) {
  // Abstraction points are locations where the analyzer may store and
  // cut abstract states. Different strategies choose them as follows:
  //  - Edges: every basic block becomes an abstraction point.
  //  - Function: only entry and EXIT are used.
  //  - Headers / Body / Backedges: identify loop back-edges and treat
  //    either headers, bodies, or both as abstraction points.
  std::set<llvm::BasicBlock *> abs_points;
  abs_points.insert(&func->getEntryBlock());
  abs_points.insert(Fragment::EXIT);

  // add blocks without predecessors to make sure all code ends up being
  // properly covered by at least one fragment
  for (auto &bb : *func) {
    if (llvm::pred_begin(&bb) == llvm::pred_end(&bb)) {
      abs_points.insert(&bb);
    }
  }

  switch (strat) {
  case Edges:
    for (auto &bb : *func)
      abs_points.insert(&bb);
    break;

  case Function:
    // entry and exit are already in
    break;

  case Headers:
  case Body:
  case Backedges:
    for (auto &edge : find_retreating(func)) {
      if (strat == Body || strat == Backedges)
        abs_points.insert(edge.first);

      if (strat == Headers || strat == Backedges)
        abs_points.insert(edge.second);
    }
    break;
  }

  return abs_points;
}

Fragment FragmentDecomposition::SubFragment(const Fragment &parent,
                                            llvm::BasicBlock *start,
                                            llvm::BasicBlock *end,
                                            bool includes_end_body) {
  // Build a sub-fragment by intersecting all edges that:
  //  - connect `start` to `end` without passing through other abstraction
  //    points, and
  //  - are already present in the parent fragment.
  //
  // This ensures that sub-fragments respect the original decomposition
  // while giving the analyzer finer-grained paths between locations.
  std::set<llvm::BasicBlock *> abs_points = {start, end, parent.getStart(),
                                             parent.getEnd()};

  auto *func = parent.getFunctionContext().getFunction();
  auto found_edges = find_edges(start, end, func, abs_points);
  auto &parent_edges = parent.edges();
  std::set<Fragment::edge> final_edges;
  std::set_intersection(found_edges.begin(), found_edges.end(),
                        parent_edges.begin(), parent_edges.end(),
                        std::inserter(final_edges, final_edges.begin()));

  return Fragment(parent.getFunctionContext(), start, end, final_edges,
                  includes_end_body);
}

Fragment FragmentDecomposition::FragmentForBody(const FunctionContext &fctx,
                                                llvm::BasicBlock *location) {
  std::vector<Fragment::edge> no_edges;
  return Fragment(fctx, location, location, no_edges, true);
}

FragmentDecomposition FragmentDecomposition::ForAbstractionPoints(
    const FunctionContext &fctx,
    const std::set<llvm::BasicBlock *> &abstraction_points) {
  assert(abstraction_points.find(&fctx.getFunction()->getEntryBlock()) !=
         abstraction_points.end());
  assert(abstraction_points.find(Fragment::EXIT) != abstraction_points.end());

  vout << "Abstraction points: " << repr(abstraction_points) << '\n';

  FragmentDecomposition fdec(fctx);

  for (llvm::BasicBlock *start : abstraction_points) {
    for (llvm::BasicBlock *end : abstraction_points) {
      auto edges =
          find_edges(start, end, fctx.getFunction(), abstraction_points);

      if (edges.size() > 0) {
        fdec.Fragments_.push_back(Fragment(fctx, start, end, edges));
      }
    }
  }

  return fdec;
}

FragmentDecomposition
FragmentDecomposition::For(const FunctionContext &fctx,
                           FragmentDecomposition::strategy strat) {
  auto abs_points = GetAbstractionPoints(fctx.getFunction(), strat);
  return ForAbstractionPoints(fctx, abs_points);
}

FragmentDecomposition FragmentDecomposition::For(const FunctionContext &fctx) {
  std::string s_str = fctx.getConfig().get<std::string>("FragmentDecomposition",
                                                        "Strategy", "Edges");

  std::map<std::string, strategy> strmap = {{"Edges", Edges},
                                            {"Function", Function},
                                            {"Headers", Headers},
                                            {"Body", Body},
                                            {"Backedges", Backedges}};

  if (strmap.find(s_str) == strmap.end())
    panic("Unknown decomposition strategy: '" + s_str + "'");

  return For(fctx, strmap[s_str]);
}

std::set<llvm::BasicBlock *> FragmentDecomposition::abstractionPoints() {
  std::set<llvm::BasicBlock *> result;

  for (auto &frag : *this) {
    result.insert(frag.getStart());
    result.insert(frag.getEnd());
  }

  return result;
}

std::ostream &operator<<(std::ostream &out, const FragmentDecomposition &fdec) {
  out << "[";

  bool needs_comma = false;
  for (const Fragment &frag : fdec) {
    if (needs_comma)
      out << ", ";

    out << frag;
    needs_comma = true;
  }

  out << "]";
  return out;
}
} // namespace symabs_ai
