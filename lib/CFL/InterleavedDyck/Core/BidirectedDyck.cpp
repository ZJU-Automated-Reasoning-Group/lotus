#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck {
namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::size_t checkedMultiply(std::size_t left, std::size_t right) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error("bidirected-Dyck edge table is too large");
  }
  return left * right;
}

class DisjointSets {
public:
  DisjointSets(std::size_t size, const std::vector<StatePair> &epsilon_edges)
      : parent_(size, kNone), size_(size, 0) {
    std::vector<std::size_t> heads(size, kNone);
    std::vector<std::size_t> targets;
    std::vector<std::size_t> next;
    targets.reserve(checkedMultiply(2, epsilon_edges.size()));
    next.reserve(targets.capacity());
    const auto add_edge = [&](std::size_t source, std::size_t target) {
      if (source >= size || target >= size) {
        throw std::out_of_range("bidirected-Dyck epsilon edge state");
      }
      const std::size_t arc = targets.size();
      targets.push_back(target);
      next.push_back(heads[source]);
      heads[source] = arc;
    };
    for (const StatePair &edge : epsilon_edges) {
      add_edge(edge.source, edge.target);
      add_edge(edge.target, edge.source);
    }

    std::vector<std::size_t> worklist;
    for (std::size_t root = 0; root < size; ++root) {
      if (parent_[root] != kNone) {
        continue;
      }
      parent_[root] = root;
      worklist.push_back(root);
      while (!worklist.empty()) {
        const std::size_t source = worklist.back();
        worklist.pop_back();
        ++size_[root];
        for (std::size_t arc = heads[source]; arc != kNone; arc = next[arc]) {
          const std::size_t target = targets[arc];
          if (parent_[target] == kNone) {
            parent_[target] = root;
            worklist.push_back(target);
          }
        }
      }
    }
  }

  std::size_t find(std::size_t element) {
    std::size_t &parent = parent_.at(element);
    if (parent != element) {
      parent = find(parent);
    }
    return parent;
  }

  std::pair<std::size_t, std::size_t> joinRoots(std::size_t first,
                                                std::size_t second) {
    first = find(first);
    second = find(second);
    if (first == second) {
      return {first, kNone};
    }
    if (size_[first] < size_[second]) {
      std::swap(first, second);
    }
    parent_[second] = first;
    size_[first] += size_[second];
    return {first, second};
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> size_;
};

struct LinkedList {
  std::size_t head = kNone;
  std::size_t tail = kNone;
  std::size_t size = 0;
};

class EdgePool {
public:
  void append(LinkedList &list, std::size_t target) {
    const std::size_t node = targets_.size();
    targets_.push_back(target);
    next_.push_back(kNone);
    appendNode(list, node);
  }

  void splice(LinkedList &destination, LinkedList &source) {
    if (source.size == 0) {
      return;
    }
    if (destination.size == 0) {
      destination = source;
    } else {
      next_[destination.tail] = source.head;
      destination.tail = source.tail;
      destination.size += source.size;
    }
    source = {};
  }

  LinkedList detach(LinkedList &list) {
    const LinkedList result = list;
    list = {};
    return result;
  }

  void appendReusedSingleton(LinkedList &destination, const LinkedList &old,
                             std::size_t target) {
    if (old.head == kNone) {
      append(destination, target);
      return;
    }
    targets_[old.head] = target;
    next_[old.head] = kNone;
    LinkedList singleton{old.head, old.head, 1};
    splice(destination, singleton);
  }

  std::size_t target(std::size_t node) const { return targets_.at(node); }
  std::size_t next(std::size_t node) const { return next_.at(node); }

private:
  void appendNode(LinkedList &list, std::size_t node) {
    if (list.size == 0) {
      list = {node, node, 1};
      return;
    }
    next_[list.tail] = node;
    list.tail = node;
    ++list.size;
  }

  std::vector<std::size_t> targets_;
  std::vector<std::size_t> next_;
};

class SolverState {
public:
  SolverState(std::size_t state_count, std::size_t label_count,
              const std::vector<StatePair> &epsilon_edges,
              const std::vector<LabeledStateEdge> &closing_edges)
      : sets_(state_count, epsilon_edges), label_count_(label_count),
        lists_(checkedMultiply(state_count, label_count)),
        queued_(lists_.size(), false), seen_(state_count, 0) {
    stats_.states = state_count;
    stats_.epsilon_edges = epsilon_edges.size();
    stats_.closing_edges = closing_edges.size();
    if (state_count != 0 && label_count == 0) {
      throw std::invalid_argument(
          "bidirected-Dyck label count must be positive");
    }
    for (const LabeledStateEdge &edge : closing_edges) {
      if (edge.source >= state_count || edge.target >= state_count) {
        throw std::out_of_range("bidirected-Dyck closing edge state");
      }
      if (edge.label >= label_count_) {
        throw std::out_of_range("bidirected-Dyck closing edge label");
      }
      pool_.append(list(sets_.find(edge.source), edge.label), edge.target);
    }
  }

  BidirectedDyckResult run() {
    const std::size_t state_count = seen_.size();
    for (std::size_t state = 0; state < state_count; ++state) {
      if (sets_.find(state) != state) {
        continue;
      }
      for (std::size_t label = 0; label < label_count_; ++label) {
        enqueue(state, label);
      }
    }

    while (!worklist_.empty()) {
      const auto [queued_state, label] = worklist_.front();
      worklist_.pop_front();
      ++stats_.worklist_pops;
      queued_[index(queued_state, label)] = false;

      const std::size_t source = sets_.find(queued_state);
      if (source != queued_state) {
        enqueue(source, label);
        continue;
      }

      LinkedList old = pool_.detach(list(source, label));
      std::vector<std::size_t> targets;
      nextGeneration();
      for (std::size_t node = old.head; node != kNone;
           node = pool_.next(node)) {
        const std::size_t target = sets_.find(pool_.target(node));
        if (seen_[target] != generation_) {
          seen_[target] = generation_;
          targets.push_back(target);
        }
      }
      if (targets.empty()) {
        continue;
      }

      std::size_t target = targets.front();
      for (std::size_t i = 1; i < targets.size(); ++i) {
        target = joinWithLists(target, targets[i]);
      }
      target = sets_.find(target);
      const std::size_t current_source = sets_.find(source);
      pool_.appendReusedSingleton(list(current_source, label), old, target);
      enqueue(current_source, label);
    }

    BidirectedDyckResult result;
    result.component.resize(state_count);
    for (std::size_t state = 0; state < state_count; ++state) {
      result.component[state] = sets_.find(state);
      stats_.components += result.component[state] == state ? 1U : 0U;
    }
    result.stats = stats_;
    return result;
  }

private:
  std::size_t index(std::size_t state, std::size_t label) const {
    return state * label_count_ + label;
  }

  LinkedList &list(std::size_t state, std::size_t label) {
    return lists_[index(state, label)];
  }

  void enqueue(std::size_t state, std::size_t label) {
    state = sets_.find(state);
    const std::size_t slot = index(state, label);
    if (lists_[slot].size >= 2 && !queued_[slot]) {
      queued_[slot] = true;
      worklist_.emplace_back(state, label);
    }
  }

  std::size_t joinWithLists(std::size_t first, std::size_t second) {
    const auto [root, removed] = sets_.joinRoots(first, second);
    if (removed == kNone) {
      return root;
    }
    ++stats_.component_unions;
    for (std::size_t label = 0; label < label_count_; ++label) {
      pool_.splice(list(root, label), list(removed, label));
      enqueue(root, label);
    }
    return root;
  }

  void nextGeneration() {
    ++generation_;
    if (generation_ == 0) {
      std::fill(seen_.begin(), seen_.end(), 0);
      generation_ = 1;
    }
  }

  DisjointSets sets_;
  std::size_t label_count_ = 0;
  std::vector<LinkedList> lists_;
  std::vector<unsigned char> queued_;
  std::deque<std::pair<std::size_t, std::size_t>> worklist_;
  EdgePool pool_;
  std::vector<std::size_t> seen_;
  std::size_t generation_ = 0;
  BidirectedDyckStats stats_;
};

} // namespace

BidirectedDyckResult BidirectedDyckComponentSolver::solve(
    std::size_t state_count, std::size_t label_count,
    const std::vector<StatePair> &epsilon_edges,
    const std::vector<LabeledStateEdge> &closing_edges) const {
  return SolverState(state_count, label_count, epsilon_edges, closing_edges)
      .run();
}

} // namespace lotus::cfl::interleaved_dyck
