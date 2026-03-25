/**
 * @file MPIRankAnalysis.cpp
 * @brief Implementation of Symbolic MPI Rank Analysis
 */

#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"

#include "Analysis/Concurrency/MPI/MPISymbol.h"

#include <deque>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace MPI;

namespace {

bool sameRankExpr(const RankExpr &lhs, const RankExpr &rhs) {
  return lhs.kind == rhs.kind && lhs.concrete_value == rhs.concrete_value &&
         lhs.range_min == rhs.range_min && lhs.range_max == rhs.range_max &&
         lhs.communicator == rhs.communicator;
}

bool sameRange(const std::pair<int, int> &lhs, const std::pair<int, int> &rhs) {
  return lhs.first == rhs.first && lhs.second == rhs.second;
}

const Value *canonicalCommunicator(const Value *value) {
  return value ? value->stripPointerCasts() : nullptr;
}

int predicateUpperBound(const MPIRankPredicate &predicate) {
  if (predicate.max_rank >= 0) {
    return predicate.max_rank;
  }
  return MPIRankAnalysis::defaultCommSizeUpperBound() - 1;
}

} // namespace

bool MPIRankPredicate::contains(int rank) const {
  if (unknown) {
    return true;
  }
  if (rank < min_rank) {
    return false;
  }
  if (!universal && max_rank >= 0 && rank > max_rank) {
    return false;
  }
  if (universal && max_rank >= 0 && rank > max_rank) {
    return false;
  }
  return excluded_ranks.count(rank) == 0;
}

bool MPIRankPredicate::mayOverlap(const MPIRankPredicate &other) const {
  if (unknown || other.unknown) {
    return true;
  }
  if (communicator && other.communicator && communicator != other.communicator) {
    return false;
  }
  const int lhs_upper = predicateUpperBound(*this);
  const int rhs_upper = predicateUpperBound(other);
  const int overlap_min = std::max(min_rank, other.min_rank);
  const int overlap_max = std::min(lhs_upper, rhs_upper);
  if (overlap_min > overlap_max) {
    return false;
  }
  for (int rank = overlap_min; rank <= overlap_max; ++rank) {
    if (contains(rank) && other.contains(rank)) {
      return true;
    }
  }
  return false;
}

bool MPIRankPredicate::mustEqual(const MPIRankPredicate &other) const {
  if (unknown || other.unknown) {
    return false;
  }
  if (communicator && other.communicator && communicator != other.communicator) {
    return false;
  }
  return isConcrete() && other.isConcrete() && min_rank == other.min_rank &&
         communicator == other.communicator;
}

std::string MPIRankPredicate::toKey() const {
  if (unknown) {
    return "rank:unknown";
  }
  std::string key = universal ? "rank:universal" : "rank:bounded";
  key += ":min=" + std::to_string(min_rank);
  key += ":max=" + std::to_string(max_rank);
  if (!excluded_ranks.empty()) {
    key += ":exclude=";
    bool first = true;
    for (int value : excluded_ranks) {
      if (!first) {
        key += ",";
      }
      first = false;
      key += std::to_string(value);
    }
  }
  return key;
}

bool RankExpr::mayEqual(const RankExpr &other) const {
  if (kind == Unknown || other.kind == Unknown) {
    return true;
  }

  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }

  if (kind == Concrete && other.kind == Range) {
    return concrete_value >= other.range_min &&
           concrete_value <= other.range_max;
  }

  if (kind == Range && other.kind == Concrete) {
    return other.concrete_value >= range_min &&
           other.concrete_value <= range_max;
  }

  if (kind == Range && other.kind == Range) {
    return !(range_max < other.range_min || other.range_max < range_min);
  }

  return true;
}

bool RankExpr::mustEqual(const RankExpr &other) const {
  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }
  return false;
}

MPIRankAnalysis::MPIRankAnalysis(Module &module) : m_module(module) {}

void MPIRankAnalysis::analyze() {
  m_value_to_rank.clear();
  m_value_to_predicate.clear();
  m_inst_rank.clear();
  m_inst_predicate.clear();
  m_inst_predicate_class.clear();
  m_inst_participant_class.clear();
  m_comm_size.clear();
  m_value_to_size_range.clear();

  identifyRankQueries();
  propagateValueFacts();
  propagateRankInfo();
  analyzeRankBranches();
  canonicalizePredicateClasses();
}

void MPIRankAnalysis::identifyRankQueries() {
  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallBase *call = dyn_cast<CallBase>(&inst)) {
          Function *callee = call->getCalledFunction();
          if (!callee) {
            continue;
          }

          std::string normalized_name =
              mpi::normalizeMPISymbolName(callee->getName());
          StringRef name = normalized_name;

          if (mpi::equalsCaseInsensitiveASCII(name, "MPI_Comm_rank")) {
            if (call->arg_size() >= 2) {
              const Value *rank_ptr = call->getArgOperand(1);
              const Value *comm = canonicalCommunicator(call->getArgOperand(0));
              m_value_to_rank[rank_ptr] = RankExpr::makeSymbolic(comm);
              m_value_to_predicate[rank_ptr] =
                  MPIRankPredicate::makeUniversal(comm);

              for (const Use &use : rank_ptr->uses()) {
                if (LoadInst *load = dyn_cast<LoadInst>(use.getUser())) {
                  if (load->getPointerOperand() == rank_ptr) {
                    m_value_to_rank[load] = RankExpr::makeSymbolic(comm);
                    m_value_to_predicate[load] =
                        MPIRankPredicate::makeUniversal(comm);
                  }
                }
              }
            }
          }

          if (mpi::equalsCaseInsensitiveASCII(name, "MPI_Comm_size")) {
            if (call->arg_size() >= 2) {
              const Value *comm = canonicalCommunicator(call->getArgOperand(0));
              const Value *size_ptr = call->getArgOperand(1);

              m_comm_size[comm] = defaultCommSizeUpperBound();
              m_value_to_size_range[size_ptr] =
                  std::make_pair(1, defaultCommSizeUpperBound());
              for (const Use &use : size_ptr->uses()) {
                const auto *load = dyn_cast<LoadInst>(use.getUser());
                if (load && load->getPointerOperand() == size_ptr) {
                  m_value_to_size_range[load] =
                      std::make_pair(1, defaultCommSizeUpperBound());
                }
              }
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::propagateValueFacts() {
  bool changed = true;
  while (changed) {
    changed = false;

    for (Function &func : m_module) {
      for (BasicBlock &bb : func) {
        for (Instruction &inst : bb) {
          auto updateRank = [&](const Value *value, const RankExpr &expr) {
            auto it = m_value_to_rank.find(value);
            if (it != m_value_to_rank.end() && sameRankExpr(it->second, expr)) {
              return;
            }
            m_value_to_rank[value] = expr;
            changed = true;
          };

          auto updatePredicate = [&](const Value *value,
                                     const MPIRankPredicate &predicate) {
            auto it = m_value_to_predicate.find(value);
            if (it != m_value_to_predicate.end() && !(it->second < predicate) &&
                !(predicate < it->second)) {
              return;
            }
            m_value_to_predicate[value] = predicate;
            changed = true;
          };

          auto updateSizeRange = [&](const Value *value,
                                     const std::pair<int, int> &range) {
            auto it = m_value_to_size_range.find(value);
            if (it != m_value_to_size_range.end() &&
                sameRange(it->second, range)) {
              return;
            }
            m_value_to_size_range[value] = range;
            changed = true;
          };

          if (const auto *store = dyn_cast<StoreInst>(&inst)) {
            const Value *pointer = store->getPointerOperand()->stripPointerCasts();
            RankExpr expr = getRankExpr(store->getValueOperand());
            if (expr.kind != RankExpr::Unknown) {
              updateRank(pointer, expr);
            }
            MPIRankPredicate predicate = getRankPredicate(store->getValueOperand());
            if (!predicate.unknown) {
              updatePredicate(pointer, predicate);
            }
            auto size_it = m_value_to_size_range.find(store->getValueOperand());
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(pointer, size_it->second);
            }
            continue;
          }

          if (const auto *load = dyn_cast<LoadInst>(&inst)) {
            const Value *pointer = load->getPointerOperand()->stripPointerCasts();
            RankExpr expr = getRankExpr(pointer);
            if (expr.kind != RankExpr::Unknown) {
              updateRank(load, expr);
            }
            MPIRankPredicate predicate = getRankPredicate(pointer);
            if (!predicate.unknown) {
              updatePredicate(load, predicate);
            }
            auto size_it = m_value_to_size_range.find(pointer);
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(load, size_it->second);
            }
            continue;
          }

          if (const auto *phi = dyn_cast<PHINode>(&inst)) {
            bool saw_rank = false;
            RankExpr merged_rank;
            bool saw_predicate = false;
            MPIRankPredicate merged_predicate;
            bool saw_size = false;
            std::pair<int, int> merged_size{0, 0};
            for (const Value *incoming : phi->incoming_values()) {
              RankExpr expr = getRankExpr(incoming);
              if (expr.kind != RankExpr::Unknown) {
                merged_rank = saw_rank ? mergeRankExpr(merged_rank, expr) : expr;
                saw_rank = true;
              }
              MPIRankPredicate predicate = getRankPredicate(incoming);
              if (!predicate.unknown) {
                merged_predicate = saw_predicate
                                       ? mergePredicate(merged_predicate,
                                                        predicate)
                                       : predicate;
                saw_predicate = true;
              }
              auto size_it = m_value_to_size_range.find(incoming);
              if (size_it != m_value_to_size_range.end()) {
                if (!saw_size) {
                  merged_size = size_it->second;
                  saw_size = true;
                } else {
                  merged_size.first =
                      std::min(merged_size.first, size_it->second.first);
                  merged_size.second =
                      std::max(merged_size.second, size_it->second.second);
                }
              }
            }
            if (saw_rank) {
              updateRank(phi, merged_rank);
            }
            if (saw_predicate) {
              updatePredicate(phi, merged_predicate);
            }
            if (saw_size) {
              updateSizeRange(phi, merged_size);
            }
            continue;
          }

          if (const auto *select = dyn_cast<SelectInst>(&inst)) {
            RankExpr lhs = getRankExpr(select->getTrueValue());
            RankExpr rhs = getRankExpr(select->getFalseValue());
            if (lhs.kind != RankExpr::Unknown || rhs.kind != RankExpr::Unknown) {
              updateRank(select, mergeRankExpr(lhs, rhs));
            }
            MPIRankPredicate lhs_pred = getRankPredicate(select->getTrueValue());
            MPIRankPredicate rhs_pred = getRankPredicate(select->getFalseValue());
            if (!lhs_pred.unknown || !rhs_pred.unknown) {
              updatePredicate(select, mergePredicate(lhs_pred, rhs_pred));
            }
            auto lhs_size = m_value_to_size_range.find(select->getTrueValue());
            auto rhs_size = m_value_to_size_range.find(select->getFalseValue());
            if (lhs_size != m_value_to_size_range.end() &&
                rhs_size != m_value_to_size_range.end()) {
              updateSizeRange(
                  select, std::make_pair(std::min(lhs_size->second.first,
                                                  rhs_size->second.first),
                                         std::max(lhs_size->second.second,
                                                  rhs_size->second.second)));
            }
            continue;
          }

          if (const auto *cast = dyn_cast<CastInst>(&inst)) {
            RankExpr expr = getRankExpr(cast->getOperand(0));
            if (expr.kind != RankExpr::Unknown) {
              updateRank(cast, expr);
            }
            MPIRankPredicate predicate = getRankPredicate(cast->getOperand(0));
            if (!predicate.unknown) {
              updatePredicate(cast, predicate);
            }
            auto size_it = m_value_to_size_range.find(cast->getOperand(0));
            if (size_it != m_value_to_size_range.end()) {
              updateSizeRange(cast, size_it->second);
            }
            continue;
          }

          if (const auto *cb = dyn_cast<CallBase>(&inst)) {
            Function *callee = cb->getCalledFunction();
            if (!callee || callee->isDeclaration()) {
              continue;
            }

            for (unsigned arg_idx = 0;
                 arg_idx < cb->arg_size() && arg_idx < callee->arg_size();
                 ++arg_idx) {
              const Argument *formal = callee->getArg(arg_idx);
              RankExpr formal_rank = getRankExpr(formal);
              if (formal_rank.kind != RankExpr::Unknown) {
                updateRank(cb->getArgOperand(arg_idx)->stripPointerCasts(),
                           formal_rank);
              }
              MPIRankPredicate formal_predicate = getRankPredicate(formal);
              if (!formal_predicate.unknown) {
                updatePredicate(cb->getArgOperand(arg_idx)->stripPointerCasts(),
                                formal_predicate);
              }
              auto formal_size = m_value_to_size_range.find(formal);
              if (formal_size != m_value_to_size_range.end()) {
                updateSizeRange(cb->getArgOperand(arg_idx)->stripPointerCasts(),
                                formal_size->second);
              }
            }

            RankExpr returned_rank;
            bool saw_returned_rank = false;
            MPIRankPredicate returned_predicate;
            bool saw_returned_predicate = false;
            std::pair<int, int> returned_size{0, 0};
            bool saw_returned_size = false;
            for (const BasicBlock &callee_bb : *callee) {
              if (const auto *ret = dyn_cast<ReturnInst>(callee_bb.getTerminator())) {
                const Value *ret_val = ret->getReturnValue();
                if (!ret_val) {
                  continue;
                }
                RankExpr ret_expr = getRankExpr(ret_val);
                if (ret_expr.kind != RankExpr::Unknown) {
                  returned_rank = saw_returned_rank
                                      ? mergeRankExpr(returned_rank, ret_expr)
                                      : ret_expr;
                  saw_returned_rank = true;
                }
                MPIRankPredicate ret_predicate = getRankPredicate(ret_val);
                if (!ret_predicate.unknown) {
                  returned_predicate =
                      saw_returned_predicate
                          ? mergePredicate(returned_predicate, ret_predicate)
                          : ret_predicate;
                  saw_returned_predicate = true;
                }
                auto ret_size = m_value_to_size_range.find(ret_val);
                if (ret_size != m_value_to_size_range.end()) {
                  if (!saw_returned_size) {
                    returned_size = ret_size->second;
                    saw_returned_size = true;
                  } else {
                    returned_size.first =
                        std::min(returned_size.first, ret_size->second.first);
                    returned_size.second =
                        std::max(returned_size.second, ret_size->second.second);
                  }
                }
              }
            }
            if (saw_returned_rank) {
              updateRank(cb, returned_rank);
            }
            if (saw_returned_predicate) {
              updatePredicate(cb, returned_predicate);
            }
            if (saw_returned_size) {
              updateSizeRange(cb, returned_size);
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::propagateRankInfo() {
  m_inst_rank.clear();
  m_inst_predicate.clear();

  for (Function &func : m_module) {
    if (func.isDeclaration() || func.empty()) {
      continue;
    }

    std::map<const BasicBlock *, MPIRankPredicate> in_predicate;
    std::deque<const BasicBlock *> worklist;
    std::set<const BasicBlock *> queued;

    const BasicBlock *entry = &func.getEntryBlock();
    in_predicate[entry] = MPIRankPredicate::makeUniversal();
    worklist.push_back(entry);
    queued.insert(entry);

    while (!worklist.empty()) {
      const BasicBlock *bb = worklist.front();
      worklist.pop_front();
      queued.erase(bb);

      MPIRankPredicate current_predicate = in_predicate[bb];
      RankExpr current_rank = predicateToRankExpr(current_predicate);
      for (const Instruction &inst : *bb) {
        m_inst_predicate[&inst] = current_predicate;
        m_inst_rank[&inst] = current_rank;
      }

      const Instruction *terminator = bb->getTerminator();
      const auto *br = dyn_cast_or_null<BranchInst>(terminator);
      if (br) {
        for (unsigned succ_idx = 0; succ_idx < br->getNumSuccessors();
             ++succ_idx) {
          const BasicBlock *succ = br->getSuccessor(succ_idx);
          MPIRankPredicate propagated = current_predicate;
          MPIRankPredicate refined = current_predicate;
          if (refinePredicateFromBranch(br, succ_idx, current_predicate,
                                        refined)) {
            propagated = refined;
          }

          auto it = in_predicate.find(succ);
          MPIRankPredicate merged =
              it == in_predicate.end() ? propagated
                                       : mergePredicate(it->second, propagated);
          bool changed = it == in_predicate.end() || (it->second < merged) ||
                         (merged < it->second);
          if (!changed) {
            continue;
          }
          in_predicate[succ] = merged;
          if (queued.insert(succ).second) {
            worklist.push_back(succ);
          }
        }
        continue;
      }

      for (const BasicBlock *succ : successors(bb)) {
        auto it = in_predicate.find(succ);
        MPIRankPredicate merged =
            it == in_predicate.end() ? current_predicate
                                     : mergePredicate(it->second,
                                                      current_predicate);
        bool changed = it == in_predicate.end() || (it->second < merged) ||
                       (merged < it->second);
        if (!changed) {
          continue;
        }
        in_predicate[succ] = merged;
        if (queued.insert(succ).second) {
          worklist.push_back(succ);
        }
      }
    }
  }
}

void MPIRankAnalysis::analyzeRankBranches() {
  auto intersectPredicate = [&](const MPIRankPredicate &lhs,
                                const MPIRankPredicate &rhs) {
    if (lhs.unknown) {
      return rhs;
    }
    if (rhs.unknown) {
      return lhs;
    }
    MPIRankPredicate refined = lhs;
    refined.communicator =
        lhs.communicator ? lhs.communicator : rhs.communicator;
    refined.unknown = false;
    refined.universal = lhs.universal && rhs.universal;
    refined.min_rank = std::max(lhs.min_rank, rhs.min_rank);
    refined.max_rank = std::min(predicateUpperBound(lhs), predicateUpperBound(rhs));
    refined.excluded_ranks.insert(rhs.excluded_ranks.begin(),
                                  rhs.excluded_ranks.end());
    if (refined.max_rank < refined.min_rank) {
      refined.max_rank = refined.min_rank;
    }
    return refined;
  };

  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      const auto *br = dyn_cast<BranchInst>(bb.getTerminator());
      if (!br || !br->isConditional()) {
        continue;
      }
      MPIRankPredicate current =
          bb.empty() ? MPIRankPredicate::makeUnknown()
                     : getRankPredicateAtInstruction(&bb.back());
      for (unsigned succ_idx = 0; succ_idx < br->getNumSuccessors(); ++succ_idx) {
        MPIRankPredicate refined;
        if (!refinePredicateFromBranch(br, succ_idx, current, refined)) {
          continue;
        }
        BasicBlock *succ = br->getSuccessor(succ_idx);
        for (Instruction &inst : *succ) {
          MPIRankPredicate existing = getRankPredicateAtInstruction(&inst);
          MPIRankPredicate tightened = intersectPredicate(existing, refined);
          m_inst_predicate[&inst] = tightened;
          m_inst_rank[&inst] = predicateToRankExpr(tightened);
        }
      }
    }
  }
}

void MPIRankAnalysis::canonicalizePredicateClasses() {
  m_inst_predicate_class.clear();
  m_inst_participant_class.clear();

  std::map<MPIRankPredicate, size_t> predicate_ids;
  size_t next_predicate_id = 1;

  std::map<MPIRankPredicate, size_t> participant_ids;
  size_t next_participant_id = 1;

  for (const auto &entry : m_inst_predicate) {
    const Instruction *inst = entry.first;
    const MPIRankPredicate &predicate = entry.second;
    auto pred_it = predicate_ids.find(predicate);
    if (pred_it == predicate_ids.end()) {
      pred_it = predicate_ids.emplace(predicate, next_predicate_id++).first;
    }
    m_inst_predicate_class[inst] = pred_it->second;

    auto part_it = participant_ids.find(predicate);
    if (part_it == participant_ids.end()) {
      part_it = participant_ids.emplace(predicate, next_participant_id++).first;
    }
    m_inst_participant_class[inst] = part_it->second;
  }
}

RankExpr MPIRankAnalysis::getRankExpr(const Value *val) const {
  auto it = m_value_to_rank.find(val);
  if (it != m_value_to_rank.end()) {
    return it->second;
  }
  MPIRankPredicate predicate = getRankPredicate(val);
  if (!predicate.unknown) {
    return predicateToRankExpr(predicate);
  }
  return RankExpr();
}

MPIRankPredicate MPIRankAnalysis::getRankPredicate(const Value *val) const {
  auto it = m_value_to_predicate.find(val);
  if (it != m_value_to_predicate.end()) {
    return it->second;
  }
  RankExpr expr = RankExpr();
  auto rank_it = m_value_to_rank.find(val);
  if (rank_it != m_value_to_rank.end()) {
    expr = rank_it->second;
    switch (expr.kind) {
    case RankExpr::Concrete:
      return MPIRankPredicate::makeConcrete(expr.concrete_value,
                                            expr.communicator);
    case RankExpr::Range:
      return MPIRankPredicate::makeRange(expr.range_min, expr.range_max,
                                         expr.communicator);
    case RankExpr::Symbolic:
      return MPIRankPredicate::makeUniversal(expr.communicator);
    case RankExpr::Unknown:
      break;
    }
  }
  return MPIRankPredicate::makeUnknown();
}

RankExpr MPIRankAnalysis::getRankAtInstruction(const Instruction *inst) const {
  auto it = m_inst_rank.find(inst);
  if (it != m_inst_rank.end()) {
    return it->second;
  }
  MPIRankPredicate predicate = getRankPredicateAtInstruction(inst);
  if (!predicate.unknown) {
    return predicateToRankExpr(predicate);
  }
  return RankExpr::makeSymbolic();
}

MPIRankPredicate
MPIRankAnalysis::getRankPredicateAtInstruction(const Instruction *inst) const {
  auto it = m_inst_predicate.find(inst);
  if (it != m_inst_predicate.end()) {
    return it->second;
  }
  return MPIRankPredicate::makeUniversal();
}

MPIRankAnalysis::ReachabilityKind
MPIRankAnalysis::getReachabilityAtInstruction(const Instruction *inst) const {
  MPIRankPredicate predicate = getRankPredicateAtInstruction(inst);
  if (predicate.unknown || !predicate.communicator) {
    return ReachabilityKind::Unknown;
  }
  return predicate.constrainsParticipants() ? ReachabilityKind::SomeRanks
                                            : ReachabilityKind::AllRanks;
}

size_t MPIRankAnalysis::getPredicateClassAtInstruction(
    const Instruction *inst) const {
  auto it = m_inst_predicate_class.find(inst);
  return it == m_inst_predicate_class.end() ? 0 : it->second;
}

size_t MPIRankAnalysis::getParticipantClassAtInstruction(
    const Instruction *inst) const {
  auto it = m_inst_participant_class.find(inst);
  return it == m_inst_participant_class.end() ? 0 : it->second;
}

bool MPIRankAnalysis::sameRank(const Instruction *i1,
                               const Instruction *i2) const {
  return getRankPredicateAtInstruction(i1).mustEqual(
      getRankPredicateAtInstruction(i2));
}

std::set<int> MPIRankAnalysis::getPossibleRanks(const Instruction *inst) const {
  std::set<int> ranks;

  MPIRankPredicate predicate = getRankPredicateAtInstruction(inst);
  if (predicate.unknown) {
    return ranks;
  }

  const int upper = predicateUpperBound(predicate);
  for (int rank = predicate.min_rank; rank <= upper; ++rank) {
    if (predicate.contains(rank)) {
      ranks.insert(rank);
    }
  }
  return ranks;
}

bool MPIRankAnalysis::dependsOnRank(const Value *val) const {
  if (!val) {
    return false;
  }

  std::deque<const Value *> worklist;
  std::set<const Value *> visited;
  worklist.push_back(val);

  while (!worklist.empty()) {
    const Value *current = worklist.front();
    worklist.pop_front();
    if (!current || !visited.insert(current).second) {
      continue;
    }

    auto rank_it = m_value_to_rank.find(current);
    if (rank_it != m_value_to_rank.end() &&
        rank_it->second.kind != RankExpr::Unknown) {
      return true;
    }

    auto predicate_it = m_value_to_predicate.find(current);
    if (predicate_it != m_value_to_predicate.end() &&
        !predicate_it->second.unknown) {
      return true;
    }

    auto size_it = m_value_to_size_range.find(current);
    if (size_it != m_value_to_size_range.end()) {
      return true;
    }

    if (const auto *inst = dyn_cast<Instruction>(current)) {
      for (const Value *operand : inst->operands()) {
        worklist.push_back(operand);
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      for (unsigned i = 0; i < ce->getNumOperands(); ++i) {
        worklist.push_back(ce->getOperand(i));
      }
    }
  }

  return false;
}

RankExpr MPIRankAnalysis::mergeRankExpr(const RankExpr &lhs,
                                        const RankExpr &rhs) const {
  if (lhs.kind == RankExpr::Unknown) {
    return rhs;
  }
  if (rhs.kind == RankExpr::Unknown) {
    return lhs;
  }
  if (lhs.kind == RankExpr::Symbolic || rhs.kind == RankExpr::Symbolic) {
    return RankExpr::makeSymbolic(lhs.communicator ? lhs.communicator
                                                   : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Concrete && rhs.kind == RankExpr::Concrete) {
    if (lhs.concrete_value == rhs.concrete_value) {
      return lhs;
    }
    return RankExpr::makeRange(std::min(lhs.concrete_value, rhs.concrete_value),
                               std::max(lhs.concrete_value, rhs.concrete_value),
                               lhs.communicator ? lhs.communicator
                                                : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Range && rhs.kind == RankExpr::Range) {
    return RankExpr::makeRange(std::min(lhs.range_min, rhs.range_min),
                               std::max(lhs.range_max, rhs.range_max),
                               lhs.communicator ? lhs.communicator
                                                : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Range && rhs.kind == RankExpr::Concrete) {
    return RankExpr::makeRange(std::min(lhs.range_min, rhs.concrete_value),
                               std::max(lhs.range_max, rhs.concrete_value),
                               lhs.communicator ? lhs.communicator
                                                : rhs.communicator);
  }
  if (lhs.kind == RankExpr::Concrete && rhs.kind == RankExpr::Range) {
    return mergeRankExpr(rhs, lhs);
  }
  return RankExpr::makeSymbolic(lhs.communicator ? lhs.communicator
                                                 : rhs.communicator);
}

MPIRankPredicate MPIRankAnalysis::mergePredicate(
    const MPIRankPredicate &lhs, const MPIRankPredicate &rhs) const {
  if (lhs.unknown) {
    return rhs;
  }
  if (rhs.unknown) {
    return lhs;
  }
  if (lhs.communicator && rhs.communicator && lhs.communicator != rhs.communicator) {
    return MPIRankPredicate::makeUnknown();
  }

  MPIRankPredicate merged;
  merged.communicator = lhs.communicator ? lhs.communicator : rhs.communicator;
  merged.unknown = false;
  merged.universal = lhs.universal || rhs.universal;
  merged.min_rank = std::min(lhs.min_rank, rhs.min_rank);
  merged.max_rank = std::max(predicateUpperBound(lhs), predicateUpperBound(rhs));

  std::set_intersection(lhs.excluded_ranks.begin(), lhs.excluded_ranks.end(),
                        rhs.excluded_ranks.begin(), rhs.excluded_ranks.end(),
                        std::inserter(merged.excluded_ranks,
                                      merged.excluded_ranks.begin()));

  if (!merged.universal && merged.max_rank < merged.min_rank) {
    merged.max_rank = merged.min_rank;
  }
  return merged;
}

RankExpr MPIRankAnalysis::predicateToRankExpr(
    const MPIRankPredicate &predicate) const {
  if (predicate.unknown) {
    return RankExpr();
  }
  if (predicate.isConcrete()) {
    return RankExpr::makeConcrete(predicate.min_rank, predicate.communicator);
  }
  if (predicate.universal && predicate.excluded_ranks.empty() &&
      predicate.max_rank < 0) {
    return RankExpr::makeSymbolic(predicate.communicator);
  }
  return RankExpr::makeRange(predicate.min_rank, predicateUpperBound(predicate),
                             predicate.communicator);
}

bool MPIRankAnalysis::refineRankFromBranch(const BranchInst *br,
                                           unsigned succ_idx, RankExpr current,
                                           RankExpr &refined) const {
  MPIRankPredicate predicate;
  switch (current.kind) {
  case RankExpr::Concrete:
    predicate = MPIRankPredicate::makeConcrete(current.concrete_value,
                                               current.communicator);
    break;
  case RankExpr::Range:
    predicate = MPIRankPredicate::makeRange(current.range_min, current.range_max,
                                            current.communicator);
    break;
  case RankExpr::Symbolic:
    predicate = MPIRankPredicate::makeUniversal(current.communicator);
    break;
  case RankExpr::Unknown:
    predicate = MPIRankPredicate::makeUnknown(current.communicator);
    break;
  }
  MPIRankPredicate refined_predicate;
  if (!refinePredicateFromBranch(br, succ_idx, predicate, refined_predicate)) {
    return false;
  }
  refined = predicateToRankExpr(refined_predicate);
  return true;
}

bool MPIRankAnalysis::refinePredicateFromBranch(
    const BranchInst *br, unsigned succ_idx, MPIRankPredicate current,
    MPIRankPredicate &refined) const {
  if (!br || !br->isConditional()) {
    return false;
  }

  const auto *cmp = dyn_cast<ICmpInst>(br->getCondition());
  if (!cmp) {
    return false;
  }

  const Value *lhs = cmp->getOperand(0);
  const Value *rhs = cmp->getOperand(1);
  const Value *rank_val = lhs;
  const Value *bound_val = rhs;
  ICmpInst::Predicate pred = cmp->getPredicate();
  if (!dependsOnRank(rank_val)) {
    rank_val = rhs;
    bound_val = lhs;
    pred = cmp->getSwappedPredicate();
  }

  if (!dependsOnRank(rank_val)) {
    return false;
  }

  const bool takes_edge = succ_idx == 0;
  refined = current.unknown ? MPIRankPredicate::makeUniversal() : current;
  if (!refined.communicator) {
    RankExpr rank_expr = getRankExpr(rank_val);
    refined.communicator = rank_expr.communicator;
  }

  int bound_min = 0;
  int bound_max = defaultCommSizeUpperBound() - 1;
  if (!tryEvaluateIntRange(bound_val, bound_min, bound_max)) {
    return false;
  }

  int current_upper = predicateUpperBound(refined);

  auto clampLower = [&](int lower) {
    refined.unknown = false;
    refined.universal = false;
    refined.min_rank = std::max(refined.min_rank, lower);
    refined.max_rank = std::max(refined.min_rank, current_upper);
    for (auto it = refined.excluded_ranks.begin(); it != refined.excluded_ranks.end();) {
      if (*it < refined.min_rank || *it > refined.max_rank) {
        it = refined.excluded_ranks.erase(it);
      } else {
        ++it;
      }
    }
  };
  auto clampUpper = [&](int upper) {
    refined.unknown = false;
    refined.universal = false;
    refined.max_rank = std::min(current_upper, upper);
    if (refined.max_rank < refined.min_rank) {
      refined.max_rank = refined.min_rank;
    }
    for (auto it = refined.excluded_ranks.begin(); it != refined.excluded_ranks.end();) {
      if (*it < refined.min_rank || *it > refined.max_rank) {
        it = refined.excluded_ranks.erase(it);
      } else {
        ++it;
      }
    }
  };

  switch (pred) {
  case CmpInst::ICMP_EQ:
    if (takes_edge && bound_min == bound_max) {
      refined = MPIRankPredicate::makeConcrete(bound_min, refined.communicator);
      return true;
    }
    if (!takes_edge && bound_min == bound_max) {
      refined.unknown = false;
      refined.excluded_ranks.insert(bound_min);
      return true;
    }
    return false;
  case CmpInst::ICMP_NE:
    if (takes_edge && bound_min == bound_max) {
      refined.unknown = false;
      refined.excluded_ranks.insert(bound_min);
      return true;
    }
    if (!takes_edge && bound_min == bound_max) {
      refined = MPIRankPredicate::makeConcrete(bound_min, refined.communicator);
      return true;
    }
    return false;
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_ULT:
    if (takes_edge) {
      clampUpper(bound_max - 1);
    } else {
      clampLower(std::max(0, bound_min));
    }
    return true;
  case CmpInst::ICMP_SLE:
  case CmpInst::ICMP_ULE:
    if (takes_edge) {
      clampUpper(bound_max);
    } else {
      clampLower(std::max(0, bound_min + 1));
    }
    return true;
  case CmpInst::ICMP_SGT:
  case CmpInst::ICMP_UGT:
    if (takes_edge) {
      clampLower(std::max(0, bound_min + 1));
    } else {
      clampUpper(bound_max);
    }
    return true;
  case CmpInst::ICMP_SGE:
  case CmpInst::ICMP_UGE:
    if (takes_edge) {
      clampLower(std::max(0, bound_min));
    } else {
      clampUpper(bound_max - 1);
    }
    return true;
  default:
    return false;
  }
}

bool MPIRankAnalysis::tryEvaluateIntRange(const Value *val, int &min_value,
                                          int &max_value) const {
  if (!val) {
    return false;
  }

  if (const auto *ci = dyn_cast<ConstantInt>(val)) {
    min_value = ci->getSExtValue();
    max_value = min_value;
    return true;
  }

  auto size_it = m_value_to_size_range.find(val);
  if (size_it != m_value_to_size_range.end()) {
    min_value = size_it->second.first;
    max_value = size_it->second.second;
    return true;
  }

  const auto *inst = dyn_cast<Instruction>(val);
  if (!inst) {
    return false;
  }

  const auto *binop = dyn_cast<BinaryOperator>(inst);
  if (!binop) {
    return false;
  }

  int lhs_min = 0;
  int lhs_max = 0;
  int rhs_min = 0;
  int rhs_max = 0;
  if (!tryEvaluateIntRange(binop->getOperand(0), lhs_min, lhs_max) ||
      !tryEvaluateIntRange(binop->getOperand(1), rhs_min, rhs_max)) {
    return false;
  }

  switch (binop->getOpcode()) {
  case Instruction::Add:
    min_value = lhs_min + rhs_min;
    max_value = lhs_max + rhs_max;
    return true;
  case Instruction::Sub:
    min_value = lhs_min - rhs_max;
    max_value = lhs_max - rhs_min;
    return true;
  default:
    return false;
  }
}

bool MPIRankAnalysis::getCommunicatorSizeRange(const Value *communicator,
                                               int &min_value,
                                               int &max_value) const {
  const Value *canonical = canonicalCommunicator(communicator);
  auto it = m_comm_size.find(canonical);
  if (it != m_comm_size.end()) {
    min_value = it->second;
    max_value = it->second;
    return true;
  }
  if (!canonical) {
    return false;
  }
  min_value = 1;
  max_value = defaultCommSizeUpperBound();
  return true;
}
