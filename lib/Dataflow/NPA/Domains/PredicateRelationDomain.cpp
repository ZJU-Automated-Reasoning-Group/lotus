#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"

#include "Solvers/CUDD/cudd.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace npa {

struct PredicateRelation::Impl {
  Impl(unsigned predicate_count_in, DdNode *root)
      : predicate_count(predicate_count_in), bdd(root) {}
  ~Impl();
  unsigned predicate_count = 0;
  DdNode *bdd = nullptr;
};

struct PredicateTensorRelation::Impl {
  Impl(unsigned predicate_count_in, DdNode *root)
      : predicate_count(predicate_count_in), bdd(root) {}
  ~Impl();
  unsigned predicate_count = 0;
  DdNode *bdd = nullptr;
};

namespace {

enum class BaseVarGroup { Next = 0, Cur = 1, Mid = 2 };
enum class TensorVarGroup {
  APrime = 0,
  A = 1,
  B = 2,
  BPrime = 3,
  TmpA = 4,
  TmpB = 5,
};

constexpr unsigned kMaxMaterializePredicatesRelation = 12;
constexpr unsigned kMaxMaterializePredicatesTensor = 6;

unsigned &configuredPredicateCountRef() {
  static unsigned count = 0;
  return count;
}

unsigned &configuredLocalPredicateCountRef() {
  static unsigned count = 0;
  return count;
}

std::vector<bool> &configuredLocalPredicateMaskRef() {
  static std::vector<bool> mask;
  return mask;
}

unsigned activePredicateCount() {
  const unsigned count = configuredPredicateCountRef();
  assert(count > 0 &&
         "PredicateRelationDomain::configure must be called first");
  return count;
}

unsigned activeLocalPredicateCount() {
  const unsigned local_count = configuredLocalPredicateCountRef();
  assert(local_count <= activePredicateCount());
  return local_count;
}

unsigned activeGlobalPredicateCount() {
  return activePredicateCount() - activeLocalPredicateCount();
}

const std::vector<bool> &activeLocalPredicateMask() {
  const auto &mask = configuredLocalPredicateMaskRef();
  assert(mask.size() == activePredicateCount() &&
         "PredicateRelationDomain::configure must initialize the local mask");
  return mask;
}

bool isConfiguredLocalPredicate(unsigned predicate) {
  const auto &mask = activeLocalPredicateMask();
  assert(predicate < mask.size());
  return mask[predicate];
}

unsigned baseVarIndex(unsigned predicate, BaseVarGroup group) {
  return predicate * 3 + static_cast<unsigned>(group);
}

unsigned tensorVarIndex(unsigned predicate, TensorVarGroup group) {
  return predicate * 6 + static_cast<unsigned>(group);
}

unsigned baseTotalVars(unsigned predicate_count) { return predicate_count * 3; }

unsigned tensorTotalVars(unsigned predicate_count) {
  return predicate_count * 6;
}

struct ManagerState {
  DdManager *base_manager = nullptr;
  DdManager *tensor_manager = nullptr;
};

ManagerState &managerState(unsigned predicate_count) {
  static std::mutex mu;
  static std::unordered_map<unsigned, ManagerState> states;
  std::lock_guard<std::mutex> lock(mu);
  return states[predicate_count];
}

template <typename Fn> DdNode *withRef(DdManager *manager, Fn &&fn) {
  (void)manager;
  DdNode *node = fn();
  Cudd_Ref(node);
  return node;
}

DdManager *getBaseManager(unsigned predicate_count) {
  ManagerState &state = managerState(predicate_count);
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (!state.base_manager) {
    state.base_manager = Cudd_Init(baseTotalVars(predicate_count), 0,
                                   CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
  }
  return state.base_manager;
}

DdManager *getTensorManager(unsigned predicate_count) {
  ManagerState &state = managerState(predicate_count);
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  if (!state.tensor_manager) {
    state.tensor_manager = Cudd_Init(tensorTotalVars(predicate_count), 0,
                                     CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS, 0);
  }
  return state.tensor_manager;
}

DdNode *baseVarAt(unsigned predicate_count, BaseVarGroup group, unsigned idx) {
  return Cudd_bddIthVar(getBaseManager(predicate_count),
                        static_cast<int>(baseVarIndex(idx, group)));
}

DdNode *tensorVarAt(unsigned predicate_count, TensorVarGroup group,
                    unsigned idx) {
  return Cudd_bddIthVar(getTensorManager(predicate_count),
                        static_cast<int>(tensorVarIndex(idx, group)));
}

DdNode *logicZero(DdManager *manager) { return Cudd_ReadLogicZero(manager); }

DdNode *logicOne(DdManager *manager) { return Cudd_ReadOne(manager); }

DdNode *bddAnd(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  return withRef(manager, [&] { return Cudd_bddAnd(manager, lhs, rhs); });
}

DdNode *bddOr(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  return withRef(manager, [&] { return Cudd_bddOr(manager, lhs, rhs); });
}

DdNode *bddXnor(DdManager *manager, DdNode *lhs, DdNode *rhs) {
  DdNode *xor_node =
      withRef(manager, [&] { return Cudd_bddXor(manager, lhs, rhs); });
  DdNode *xnor_node = withRef(manager, [&] { return Cudd_Not(xor_node); });
  Cudd_RecursiveDeref(manager, xor_node);
  return xnor_node;
}

template <class VarFn>
DdNode *cubeForVars(DdManager *manager, unsigned predicate_count,
                    VarFn &&var_fn) {
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(predicate_count);
  phase.assign(predicate_count, 1);
  for (unsigned i = 0; i < predicate_count; ++i)
    vars.push_back(var_fn(i));
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *relationIdentityNode(unsigned predicate_count) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *eq =
        bddXnor(manager, baseVarAt(predicate_count, BaseVarGroup::Cur, i),
                baseVarAt(predicate_count, BaseVarGroup::Next, i));
    DdNode *tmp = bddAnd(manager, node, eq);
    Cudd_RecursiveDeref(manager, eq);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

DdNode *tensorIdentityNode(unsigned predicate_count) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *eq_left = bddXnor(
        manager, tensorVarAt(predicate_count, TensorVarGroup::APrime, i),
        tensorVarAt(predicate_count, TensorVarGroup::A, i));
    DdNode *tmp = bddAnd(manager, node, eq_left);
    Cudd_RecursiveDeref(manager, eq_left);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;

    DdNode *eq_right =
        bddXnor(manager, tensorVarAt(predicate_count, TensorVarGroup::B, i),
                tensorVarAt(predicate_count, TensorVarGroup::BPrime, i));
    tmp = bddAnd(manager, node, eq_right);
    Cudd_RecursiveDeref(manager, eq_right);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

template <typename VisitFn>
void enumerateBitVectors(unsigned width, VisitFn &&visit);
std::vector<int> assignmentVector(unsigned predicate_count);
std::vector<int> tensorAssignmentVector(unsigned predicate_count);
void setAssignment(std::vector<int> &values, unsigned predicate_count,
                   BaseVarGroup group, std::uint64_t bits);
void setTensorAssignment(std::vector<int> &values, unsigned predicate_count,
                         TensorVarGroup group, std::uint64_t bits);
template <typename GroupFn>
DdNode *transitionCubeGeneric(DdManager *manager, unsigned predicate_count,
                              GroupFn &&group_fn);
template <typename MapperFn>
DdNode *remapBddBetweenManagers(DdManager *src_manager, DdManager *dst_manager,
                                DdNode *node, unsigned src_var_count,
                                MapperFn &&mapper);

template <typename GroupEnum>
std::vector<DdNode *> varsForGroupRange(unsigned predicate_count,
                                        GroupEnum group, unsigned begin,
                                        unsigned end);
DdNode *baseGroupCube(unsigned predicate_count,
                      const std::vector<BaseVarGroup> &groups,
                      unsigned begin = 0, unsigned end = 0);
DdNode *tensorGroupCube(unsigned predicate_count,
                        const std::vector<TensorVarGroup> &groups,
                        unsigned begin = 0, unsigned end = 0);
DdNode *tensorEqualityNode(unsigned predicate_count, TensorVarGroup lhs,
                           TensorVarGroup rhs, unsigned begin = 0,
                           unsigned end = 0);
template <typename GroupEnum>
DdNode *
swapVariableGroups(DdManager *manager, DdNode *node, unsigned predicate_count,
                   const std::vector<std::pair<GroupEnum, GroupEnum>> &pairs);
DdNode *remapBaseBddToTensor(DdNode *node, unsigned predicate_count,
                             BaseVarGroup next_group, BaseVarGroup cur_group,
                             TensorVarGroup mapped_next,
                             TensorVarGroup mapped_cur);
DdNode *remapTensorBddToBase(DdNode *node, unsigned predicate_count,
                             TensorVarGroup mapped_cur,
                             TensorVarGroup mapped_next);

DdNode *baseComposeNode(unsigned predicate_count, DdNode *outer,
                        DdNode *inner) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *outer_mid = swapVariableGroups<BaseVarGroup>(
      manager, outer, predicate_count,
      {{BaseVarGroup::Next, BaseVarGroup::Mid}});
  DdNode *inner_mid = swapVariableGroups<BaseVarGroup>(
      manager, inner, predicate_count,
      {{BaseVarGroup::Cur, BaseVarGroup::Mid}});
  DdNode *joined = bddAnd(manager, outer_mid, inner_mid);
  DdNode *mid_cube = baseGroupCube(predicate_count, {BaseVarGroup::Mid});
  DdNode *result = withRef(manager, [&] {
    return Cudd_bddExistAbstract(manager, joined, mid_cube);
  });
  Cudd_RecursiveDeref(manager, outer_mid);
  Cudd_RecursiveDeref(manager, inner_mid);
  Cudd_RecursiveDeref(manager, joined);
  Cudd_RecursiveDeref(manager, mid_cube);
  return result;
}

DdNode *tensorComposeNode(unsigned predicate_count, DdNode *outer,
                          DdNode *inner) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *outer_tmp = swapVariableGroups<TensorVarGroup>(
      manager, outer, predicate_count,
      {{TensorVarGroup::A, TensorVarGroup::TmpA},
       {TensorVarGroup::BPrime, TensorVarGroup::TmpB}});
  DdNode *inner_tmp = swapVariableGroups<TensorVarGroup>(
      manager, inner, predicate_count,
      {{TensorVarGroup::APrime, TensorVarGroup::TmpA},
       {TensorVarGroup::B, TensorVarGroup::TmpB}});
  DdNode *joined = bddAnd(manager, outer_tmp, inner_tmp);
  DdNode *tmp_cube = tensorGroupCube(
      predicate_count, {TensorVarGroup::TmpA, TensorVarGroup::TmpB});
  DdNode *result = withRef(manager, [&] {
    return Cudd_bddExistAbstract(manager, joined, tmp_cube);
  });
  Cudd_RecursiveDeref(manager, outer_tmp);
  Cudd_RecursiveDeref(manager, inner_tmp);
  Cudd_RecursiveDeref(manager, joined);
  Cudd_RecursiveDeref(manager, tmp_cube);
  return result;
}

template <typename VisitFn>
void enumerateBitVectors(unsigned width, VisitFn &&visit) {
  assert(width < 64 && "bit-vector enumeration width must be < 64");
  const std::uint64_t total = (std::uint64_t{1} << width);
  if (width == 0) {
    visit(std::uint64_t{0});
    return;
  }
  for (std::uint64_t value = 0; value < total; ++value)
    visit(value);
}

std::vector<int> assignmentVector(unsigned predicate_count) {
  return std::vector<int>(baseTotalVars(predicate_count), 0);
}

std::vector<int> tensorAssignmentVector(unsigned predicate_count) {
  return std::vector<int>(tensorTotalVars(predicate_count), 0);
}

void setAssignment(std::vector<int> &values, unsigned predicate_count,
                   BaseVarGroup group, std::uint64_t bits) {
  for (unsigned i = 0; i < predicate_count; ++i)
    values[baseVarIndex(i, group)] = (bits >> i) & 1U;
}

void setTensorAssignment(std::vector<int> &values, unsigned predicate_count,
                         TensorVarGroup group, std::uint64_t bits) {
  for (unsigned i = 0; i < predicate_count; ++i)
    values[tensorVarIndex(i, group)] = (bits >> i) & 1U;
}

template <typename GroupFn>
DdNode *transitionCubeGeneric(DdManager *manager, unsigned predicate_count,
                              GroupFn &&group_fn) {
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    const auto lits = group_fn(i);
    for (const auto &lit : lits) {
      DdNode *var = nullptr;
      if (lit.second == 0 || lit.second == 1) {
        var = lit.second ? lit.first : Cudd_Not(lit.first);
      } else {
        continue;
      }
      DdNode *lit_ref = withRef(manager, [&] { return var; });
      DdNode *tmp = bddAnd(manager, node, lit_ref);
      Cudd_RecursiveDeref(manager, lit_ref);
      Cudd_RecursiveDeref(manager, node);
      node = tmp;
    }
  }
  return node;
}

template <typename GroupEnum>
std::vector<DdNode *> varsForGroupRange(unsigned predicate_count,
                                        GroupEnum group, unsigned begin,
                                        unsigned end) {
  if (end == 0 || end > predicate_count)
    end = predicate_count;
  std::vector<DdNode *> vars;
  vars.reserve(end - begin);
  for (unsigned i = begin; i < end; ++i) {
    if (std::is_same<GroupEnum, BaseVarGroup>::value)
      vars.push_back(
          baseVarAt(predicate_count, static_cast<BaseVarGroup>(group), i));
    else
      vars.push_back(
          tensorVarAt(predicate_count, static_cast<TensorVarGroup>(group), i));
  }
  return vars;
}

DdNode *baseGroupCube(unsigned predicate_count,
                      const std::vector<BaseVarGroup> &groups, unsigned begin,
                      unsigned end) {
  DdManager *manager = getBaseManager(predicate_count);
  if (end == 0 || end > predicate_count)
    end = predicate_count;
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(groups.size() * (end - begin));
  phase.reserve(groups.size() * (end - begin));
  for (BaseVarGroup group : groups) {
    for (unsigned i = begin; i < end; ++i) {
      vars.push_back(baseVarAt(predicate_count, group, i));
      phase.push_back(1);
    }
  }
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *baseGroupCubeForIndices(unsigned predicate_count,
                                const std::vector<BaseVarGroup> &groups,
                                const std::vector<unsigned> &indices) {
  DdManager *manager = getBaseManager(predicate_count);
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(groups.size() * indices.size());
  phase.reserve(groups.size() * indices.size());
  for (BaseVarGroup group : groups) {
    for (unsigned idx : indices) {
      vars.push_back(baseVarAt(predicate_count, group, idx));
      phase.push_back(1);
    }
  }
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *tensorGroupCube(unsigned predicate_count,
                        const std::vector<TensorVarGroup> &groups,
                        unsigned begin, unsigned end) {
  DdManager *manager = getTensorManager(predicate_count);
  if (end == 0 || end > predicate_count)
    end = predicate_count;
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(groups.size() * (end - begin));
  phase.reserve(groups.size() * (end - begin));
  for (TensorVarGroup group : groups) {
    for (unsigned i = begin; i < end; ++i) {
      vars.push_back(tensorVarAt(predicate_count, group, i));
      phase.push_back(1);
    }
  }
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *tensorGroupCubeForIndices(unsigned predicate_count,
                                  const std::vector<TensorVarGroup> &groups,
                                  const std::vector<unsigned> &indices) {
  DdManager *manager = getTensorManager(predicate_count);
  std::vector<DdNode *> vars;
  std::vector<int> phase;
  vars.reserve(groups.size() * indices.size());
  phase.reserve(groups.size() * indices.size());
  for (TensorVarGroup group : groups) {
    for (unsigned idx : indices) {
      vars.push_back(tensorVarAt(predicate_count, group, idx));
      phase.push_back(1);
    }
  }
  return withRef(manager, [&] {
    return Cudd_bddComputeCube(manager, vars.data(), phase.data(),
                               static_cast<int>(vars.size()));
  });
}

DdNode *baseEqualityNodeForIndices(unsigned predicate_count, BaseVarGroup lhs,
                                   BaseVarGroup rhs,
                                   const std::vector<unsigned> &indices) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned idx : indices) {
    DdNode *eq = bddXnor(manager, baseVarAt(predicate_count, lhs, idx),
                         baseVarAt(predicate_count, rhs, idx));
    DdNode *tmp = bddAnd(manager, node, eq);
    Cudd_RecursiveDeref(manager, eq);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

DdNode *tensorEqualityNode(unsigned predicate_count, TensorVarGroup lhs,
                           TensorVarGroup rhs, unsigned begin, unsigned end) {
  DdManager *manager = getTensorManager(predicate_count);
  if (end == 0 || end > predicate_count)
    end = predicate_count;
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = begin; i < end; ++i) {
    DdNode *eq = bddXnor(manager, tensorVarAt(predicate_count, lhs, i),
                         tensorVarAt(predicate_count, rhs, i));
    DdNode *tmp = bddAnd(manager, node, eq);
    Cudd_RecursiveDeref(manager, eq);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

DdNode *tensorEqualityNodeForIndices(unsigned predicate_count,
                                     TensorVarGroup lhs, TensorVarGroup rhs,
                                     const std::vector<unsigned> &indices) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned idx : indices) {
    DdNode *eq = bddXnor(manager, tensorVarAt(predicate_count, lhs, idx),
                         tensorVarAt(predicate_count, rhs, idx));
    DdNode *tmp = bddAnd(manager, node, eq);
    Cudd_RecursiveDeref(manager, eq);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return node;
}

template <typename GroupEnum>
DdNode *
swapVariableGroups(DdManager *manager, DdNode *node, unsigned predicate_count,
                   const std::vector<std::pair<GroupEnum, GroupEnum>> &pairs) {
  std::vector<int> permutation(
      static_cast<std::size_t>(Cudd_ReadSize(manager)));
  for (int i = 0; i < Cudd_ReadSize(manager); ++i)
    permutation[static_cast<std::size_t>(i)] = i;
  for (const auto &pair : pairs) {
    for (unsigned i = 0; i < predicate_count; ++i) {
      int lhs = 0;
      int rhs = 0;
      if (std::is_same<GroupEnum, BaseVarGroup>::value) {
        lhs = static_cast<int>(
            baseVarIndex(i, static_cast<BaseVarGroup>(pair.first)));
        rhs = static_cast<int>(
            baseVarIndex(i, static_cast<BaseVarGroup>(pair.second)));
      } else {
        lhs = static_cast<int>(
            tensorVarIndex(i, static_cast<TensorVarGroup>(pair.first)));
        rhs = static_cast<int>(
            tensorVarIndex(i, static_cast<TensorVarGroup>(pair.second)));
      }
      std::swap(permutation[static_cast<std::size_t>(lhs)],
                permutation[static_cast<std::size_t>(rhs)]);
    }
  }
  return remapBddBetweenManagers(
      manager, manager, node, static_cast<unsigned>(Cudd_ReadSize(manager)),
      [&](unsigned src_index) { return permutation.at(src_index); });
}

DdNode *remapBddRecursive(DdManager *src_manager, DdManager *dst_manager,
                          DdNode *node, const std::vector<int> &index_map,
                          std::unordered_map<DdNode *, DdNode *> &memo) {
  const bool complemented = Cudd_IsComplement(node);
  DdNode *regular = Cudd_Regular(node);
  if (Cudd_IsConstant(regular)) {
    DdNode *base = regular == Cudd_ReadOne(src_manager)
                       ? logicOne(dst_manager)
                       : logicZero(dst_manager);
    DdNode *out = complemented ? Cudd_Not(base) : base;
    Cudd_Ref(out);
    return out;
  }

  auto it = memo.find(regular);
  if (it != memo.end()) {
    DdNode *out = complemented ? Cudd_Not(it->second) : it->second;
    Cudd_Ref(out);
    return out;
  }

  const unsigned src_index = Cudd_NodeReadIndex(regular);
  assert(src_index < index_map.size() && index_map[src_index] >= 0);
  DdNode *var = Cudd_bddIthVar(dst_manager, index_map[src_index]);
  DdNode *then_branch = remapBddRecursive(src_manager, dst_manager,
                                          Cudd_T(regular), index_map, memo);
  DdNode *else_branch = remapBddRecursive(src_manager, dst_manager,
                                          Cudd_E(regular), index_map, memo);
  DdNode *rebuilt = withRef(dst_manager, [&] {
    return Cudd_bddIte(dst_manager, var, then_branch, else_branch);
  });
  Cudd_RecursiveDeref(dst_manager, then_branch);
  Cudd_RecursiveDeref(dst_manager, else_branch);
  memo.emplace(regular, rebuilt);

  DdNode *out = complemented ? Cudd_Not(rebuilt) : rebuilt;
  Cudd_Ref(out);
  return out;
}

template <typename MapperFn>
DdNode *remapBddBetweenManagers(DdManager *src_manager, DdManager *dst_manager,
                                DdNode *node, unsigned src_var_count,
                                MapperFn &&mapper) {
  std::vector<int> index_map(src_var_count, -1);
  for (unsigned i = 0; i < src_var_count; ++i)
    index_map[i] = mapper(i);
  std::unordered_map<DdNode *, DdNode *> memo;
  DdNode *result =
      remapBddRecursive(src_manager, dst_manager, node, index_map, memo);
  for (const auto &entry : memo)
    Cudd_RecursiveDeref(dst_manager, entry.second);
  return result;
}

DdNode *remapBaseBddToTensor(DdNode *node, unsigned predicate_count,
                             BaseVarGroup next_group, BaseVarGroup cur_group,
                             TensorVarGroup mapped_next,
                             TensorVarGroup mapped_cur) {
  return remapBddBetweenManagers(
      getBaseManager(predicate_count), getTensorManager(predicate_count), node,
      baseTotalVars(predicate_count), [&](unsigned src_index) {
        const unsigned predicate = src_index / 3;
        const unsigned group = src_index % 3;
        if (group == static_cast<unsigned>(next_group))
          return static_cast<int>(tensorVarIndex(predicate, mapped_next));
        if (group == static_cast<unsigned>(cur_group))
          return static_cast<int>(tensorVarIndex(predicate, mapped_cur));
        return -1;
      });
}

DdNode *remapTensorBddToBase(DdNode *node, unsigned predicate_count,
                             TensorVarGroup mapped_cur,
                             TensorVarGroup mapped_next) {
  return remapBddBetweenManagers(
      getTensorManager(predicate_count), getBaseManager(predicate_count), node,
      tensorTotalVars(predicate_count), [&](unsigned src_index) {
        const unsigned predicate = src_index / 6;
        const unsigned group = src_index % 6;
        if (group == static_cast<unsigned>(mapped_cur))
          return static_cast<int>(baseVarIndex(predicate, BaseVarGroup::Cur));
        if (group == static_cast<unsigned>(mapped_next))
          return static_cast<int>(baseVarIndex(predicate, BaseVarGroup::Next));
        return -1;
      });
}

std::shared_ptr<PredicateRelation::Impl>
makeRelationImpl(unsigned predicate_count, DdNode *node) {
  return std::make_shared<PredicateRelation::Impl>(predicate_count, node);
}

std::shared_ptr<PredicateTensorRelation::Impl>
makeTensorImpl(unsigned predicate_count, DdNode *node) {
  return std::make_shared<PredicateTensorRelation::Impl>(predicate_count, node);
}

PredicateRelation relationFromNode(unsigned predicate_count, DdNode *node) {
  return PredicateRelation(makeRelationImpl(predicate_count, node));
}

PredicateTensorRelation tensorFromNode(unsigned predicate_count, DdNode *node) {
  return PredicateTensorRelation(makeTensorImpl(predicate_count, node));
}

const std::shared_ptr<PredicateRelation::Impl> &
implOf(const PredicateRelation &value) {
  return value.impl;
}

const std::shared_ptr<PredicateTensorRelation::Impl> &
implOf(const PredicateTensorRelation &value) {
  return value.impl;
}

PredicateRelation relationFromTransitionsImpl(
    unsigned predicate_count,
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> &transitions) {
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  for (const auto &transition : transitions) {
    DdNode *cube =
        transitionCubeGeneric(manager, predicate_count, [&](unsigned idx) {
          return std::array<std::pair<DdNode *, int>, 2>{
              std::make_pair(baseVarAt(predicate_count, BaseVarGroup::Cur, idx),
                             static_cast<int>((transition.first >> idx) & 1U)),
              std::make_pair(
                  baseVarAt(predicate_count, BaseVarGroup::Next, idx),
                  static_cast<int>((transition.second >> idx) & 1U))};
        });
    DdNode *merged = bddOr(manager, result, cube);
    Cudd_RecursiveDeref(manager, cube);
    Cudd_RecursiveDeref(manager, result);
    result = merged;
  }
  return relationFromNode(predicate_count, result);
}

PredicateTensorRelation tensorFromTransitionsImpl(
    unsigned predicate_count,
    const std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t>> &transitions) {
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *result = logicZero(manager);
  Cudd_Ref(result);
  for (const auto &transition : transitions) {
    DdNode *cube =
        transitionCubeGeneric(manager, predicate_count, [&](unsigned idx) {
          return std::array<std::pair<DdNode *, int>, 4>{
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::APrime, idx),
                  static_cast<int>((std::get<0>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::B, idx),
                  static_cast<int>((std::get<1>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::A, idx),
                  static_cast<int>((std::get<2>(transition) >> idx) & 1U)),
              std::make_pair(
                  tensorVarAt(predicate_count, TensorVarGroup::BPrime, idx),
                  static_cast<int>((std::get<3>(transition) >> idx) & 1U))};
        });
    DdNode *merged = bddOr(manager, result, cube);
    Cudd_RecursiveDeref(manager, cube);
    Cudd_RecursiveDeref(manager, result);
    result = merged;
  }
  return tensorFromNode(predicate_count, result);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
materializeRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  assert(predicate_count <= 63 &&
         "materialize() supports at most 63 predicates");
  if (predicate_count > kMaxMaterializePredicatesRelation) {
    throw std::runtime_error(
        "materialize() refused: exact enumeration is infeasible for this "
        "predicate count");
  }
  DdManager *manager = getBaseManager(predicate_count);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
  std::vector<int> values = assignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t cur_bits) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t next_bits) {
      std::fill(values.begin(), values.end(), 0);
      setAssignment(values, predicate_count, BaseVarGroup::Cur, cur_bits);
      setAssignment(values, predicate_count, BaseVarGroup::Next, next_bits);
      if (Cudd_Eval(manager, implOf(relation)->bdd, values.data()) !=
          logicZero(manager)) {
        out.emplace_back(cur_bits, next_bits);
      }
    });
  });
  return out;
}

std::vector<
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
materializeTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  assert(predicate_count <= 63 &&
         "tensor materialization supports at most 63 predicates");
  if (predicate_count > kMaxMaterializePredicatesTensor) {
    throw std::runtime_error(
        "tensor materialize() refused: exact enumeration is infeasible for "
        "this predicate count");
  }
  DdManager *manager = getTensorManager(predicate_count);
  std::vector<
      std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
      out;
  std::vector<int> values = tensorAssignmentVector(predicate_count);
  enumerateBitVectors(predicate_count, [&](std::uint64_t a_prime) {
    enumerateBitVectors(predicate_count, [&](std::uint64_t b) {
      enumerateBitVectors(predicate_count, [&](std::uint64_t a) {
        enumerateBitVectors(predicate_count, [&](std::uint64_t b_prime) {
          std::fill(values.begin(), values.end(), 0);
          setTensorAssignment(values, predicate_count, TensorVarGroup::APrime,
                              a_prime);
          setTensorAssignment(values, predicate_count, TensorVarGroup::B, b);
          setTensorAssignment(values, predicate_count, TensorVarGroup::A, a);
          setTensorAssignment(values, predicate_count, TensorVarGroup::BPrime,
                              b_prime);
          if (Cudd_Eval(manager, implOf(relation)->bdd, values.data()) !=
              logicZero(manager)) {
            out.emplace_back(a_prime, b, a, b_prime);
          }
        });
      });
    });
  });
  return out;
}

PredicateRelation transposeRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  return relationFromNode(predicate_count,
                          swapVariableGroups<BaseVarGroup>(
                              getBaseManager(predicate_count),
                              implOf(relation)->bdd, predicate_count,
                              {{BaseVarGroup::Cur, BaseVarGroup::Next}}));
}

PredicateTensorRelation coupleRelationImpl(const PredicateRelation &lhs,
                                           const PredicateRelation &rhs) {
  const unsigned predicate_count = implOf(lhs)->predicate_count;
  assert(predicate_count == implOf(rhs)->predicate_count);
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *lhs_tensor = remapBaseBddToTensor(
      implOf(lhs)->bdd, predicate_count, BaseVarGroup::Next, BaseVarGroup::Cur,
      TensorVarGroup::APrime, TensorVarGroup::A);
  DdNode *rhs_tensor = remapBaseBddToTensor(
      implOf(rhs)->bdd, predicate_count, BaseVarGroup::Next, BaseVarGroup::Cur,
      TensorVarGroup::BPrime, TensorVarGroup::B);
  DdNode *coupled = bddAnd(manager, lhs_tensor, rhs_tensor);
  Cudd_RecursiveDeref(manager, lhs_tensor);
  Cudd_RecursiveDeref(manager, rhs_tensor);
  return tensorFromNode(predicate_count, coupled);
}

PredicateRelation readoutTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *eq = tensorEqualityNode(predicate_count, TensorVarGroup::APrime,
                                  TensorVarGroup::B);
  DdNode *restricted = bddAnd(manager, implOf(relation)->bdd, eq);
  DdNode *cube = tensorGroupCube(predicate_count,
                                 {TensorVarGroup::APrime, TensorVarGroup::B});
  DdNode *abstracted = withRef(manager, [&] {
    return Cudd_bddExistAbstract(manager, restricted, cube);
  });
  Cudd_RecursiveDeref(manager, eq);
  Cudd_RecursiveDeref(manager, restricted);
  Cudd_RecursiveDeref(manager, cube);
  DdNode *base = remapTensorBddToBase(
      abstracted, predicate_count, TensorVarGroup::A, TensorVarGroup::BPrime);
  Cudd_RecursiveDeref(manager, abstracted);
  return relationFromNode(predicate_count, base);
}

std::vector<unsigned> localPredicateIndices(unsigned predicate_count) {
  std::vector<unsigned> indices;
  const auto &mask = activeLocalPredicateMask();
  indices.reserve(activeLocalPredicateCount());
  for (unsigned predicate = 0; predicate < predicate_count; ++predicate) {
    if (mask[predicate])
      indices.push_back(predicate);
  }
  return indices;
}

PredicateRelation projectRelationImpl(const PredicateRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  const unsigned local_count = activeLocalPredicateCount();
  if (local_count == 0)
    return relation;
  const auto local_indices = localPredicateIndices(predicate_count);
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *cube = baseGroupCubeForIndices(
      predicate_count, {BaseVarGroup::Cur, BaseVarGroup::Next}, local_indices);
  DdNode *abstracted = withRef(manager, [&] {
    return Cudd_bddExistAbstract(manager, implOf(relation)->bdd, cube);
  });
  DdNode *eq = baseEqualityNodeForIndices(predicate_count, BaseVarGroup::Cur,
                                          BaseVarGroup::Next, local_indices);
  DdNode *projected = bddAnd(manager, abstracted, eq);
  Cudd_RecursiveDeref(manager, cube);
  Cudd_RecursiveDeref(manager, abstracted);
  Cudd_RecursiveDeref(manager, eq);
  return relationFromNode(predicate_count, projected);
}

PredicateTensorRelation
projectTensorImpl(const PredicateTensorRelation &relation) {
  const unsigned predicate_count = implOf(relation)->predicate_count;
  const unsigned local_count = activeLocalPredicateCount();
  if (local_count == 0)
    return relation;
  const auto local_indices = localPredicateIndices(predicate_count);
  DdManager *manager = getTensorManager(predicate_count);
  DdNode *match_in =
      tensorEqualityNodeForIndices(predicate_count, TensorVarGroup::APrime,
                                   TensorVarGroup::B, local_indices);
  DdNode *restricted = bddAnd(manager, implOf(relation)->bdd, match_in);
  DdNode *cube =
      tensorGroupCubeForIndices(predicate_count,
                                {TensorVarGroup::APrime, TensorVarGroup::B,
                                 TensorVarGroup::A, TensorVarGroup::BPrime},
                                local_indices);
  DdNode *abstracted = withRef(manager, [&] {
    return Cudd_bddExistAbstract(manager, restricted, cube);
  });
  DdNode *left_eq =
      tensorEqualityNodeForIndices(predicate_count, TensorVarGroup::APrime,
                                   TensorVarGroup::A, local_indices);
  DdNode *right_eq =
      tensorEqualityNodeForIndices(predicate_count, TensorVarGroup::B,
                                   TensorVarGroup::BPrime, local_indices);
  DdNode *eq_out = bddAnd(manager, left_eq, right_eq);
  DdNode *projected = bddAnd(manager, abstracted, eq_out);
  Cudd_RecursiveDeref(manager, match_in);
  Cudd_RecursiveDeref(manager, restricted);
  Cudd_RecursiveDeref(manager, cube);
  Cudd_RecursiveDeref(manager, abstracted);
  Cudd_RecursiveDeref(manager, left_eq);
  Cudd_RecursiveDeref(manager, right_eq);
  Cudd_RecursiveDeref(manager, eq_out);
  return tensorFromNode(predicate_count, projected);
}

std::string currentPredicateConfigurationKey() {
  const unsigned predicate_count = PredicateRelationDomain::getPredicateCount();
  std::string key = std::to_string(predicate_count) + ":";
  for (unsigned predicate = 0; predicate < predicate_count; ++predicate)
    key.push_back(PredicateRelationDomain::isLocalPredicate(predicate) ? 'L'
                                                                       : 'G');
  return key;
}

std::vector<PredicateRelation> validationBaseSamples() {
  std::vector<PredicateRelation> samples;
  samples.push_back(PredicateRelationDomain::zero());
  samples.push_back(PredicateRelationDomain::one());

  const unsigned predicate_count = PredicateRelationDomain::getPredicateCount();
  for (unsigned predicate = 0; predicate < predicate_count; ++predicate) {
    samples.push_back(PredicateRelationDomain::assume(predicate, false));
    samples.push_back(PredicateRelationDomain::assume(predicate, true));
    samples.push_back(PredicateRelationDomain::assignConst(predicate, false));
    samples.push_back(PredicateRelationDomain::assignConst(predicate, true));
  }
  return samples;
}

std::vector<PredicateTensorRelation>
validationTensorSamples(const std::vector<PredicateRelation> &base_samples) {
  std::vector<PredicateTensorRelation> samples;
  for (const auto &lhs : base_samples)
    for (const auto &rhs : base_samples)
      samples.push_back(PredicateTensorDomain::couple(lhs, rhs));
  return samples;
}

bool validatePredicateTensorCoreLaws(
    const std::vector<PredicateRelation> &base,
    const std::vector<PredicateTensorRelation> &tensor) {
  for (const auto &value : base) {
    if (!PredicateRelationDomain::equal(
            PredicateTensorDomain::readout(
                TensorSemiringTraits<PredicateRelationDomain>::right_constant(
                    value)),
            value)) {
      return false;
    }
    if (!PredicateRelationDomain::equal(
            PredicateTensorDomain::readout(
                TensorSemiringTraits<PredicateRelationDomain>::left_constant(
                    value)),
            value)) {
      return false;
    }
    if (!PredicateRelationDomain::equal(
            PredicateRelationDomain::transpose(
                PredicateRelationDomain::transpose(value)),
            value)) {
      return false;
    }
  }

  for (const auto &lhs : base) {
    for (const auto &rhs : base) {
      if (!PredicateRelationDomain::equal(
              PredicateTensorDomain::readout(
                  PredicateTensorDomain::couple(lhs, rhs)),
              PredicateRelationDomain::extend(lhs, rhs))) {
        return false;
      }
      if (!PredicateRelationDomain::equal(
              PredicateRelationDomain::transpose(
                  PredicateRelationDomain::extend(lhs, rhs)),
              PredicateRelationDomain::extend(
                  PredicateRelationDomain::transpose(rhs),
                  PredicateRelationDomain::transpose(lhs)))) {
        return false;
      }
    }
  }

  for (const auto &lhs : tensor) {
    if (!PredicateTensorDomain::equal(PredicateTensorDomain::projectT(
                                          PredicateTensorDomain::projectT(lhs)),
                                      PredicateTensorDomain::projectT(lhs))) {
      return false;
    }
    for (const auto &rhs : tensor) {
      if (!PredicateRelationDomain::equal(
              PredicateTensorDomain::readout(
                  PredicateTensorDomain::combine(lhs, rhs)),
              PredicateRelationDomain::combine(
                  PredicateTensorDomain::readout(lhs),
                  PredicateTensorDomain::readout(rhs)))) {
        return false;
      }
    }
  }

  for (const auto &a : base) {
    for (const auto &b : base) {
      for (const auto &c : base) {
        for (const auto &d : base) {
          const auto lhs = PredicateTensorDomain::extend(
              PredicateTensorDomain::couple(a, b),
              PredicateTensorDomain::couple(c, d));
          const auto rhs = PredicateTensorDomain::couple(
              PredicateRelationDomain::extend(c, a),
              PredicateRelationDomain::extend(b, d));
          if (!PredicateTensorDomain::equal(lhs, rhs))
            return false;
          if (!PredicateRelationDomain::equal(
                  PredicateTensorDomain::readout(lhs),
                  PredicateRelationDomain::extend(
                      PredicateRelationDomain::extend(c, a),
                      PredicateRelationDomain::extend(b, d)))) {
            return false;
          }
        }
      }
    }
  }

  return true;
}

bool validatePredicateTensorProjectionLaws(
    const std::vector<PredicateRelation> &base,
    const std::vector<PredicateTensorRelation> &tensor) {
  for (const auto &lhs : base) {
    for (const auto &rhs : base) {
      if (!PredicateRelationDomain::equal(
              PredicateRelationDomain::project(
                  PredicateRelationDomain::combine(lhs, rhs)),
              PredicateRelationDomain::combine(
                  PredicateRelationDomain::project(lhs),
                  PredicateRelationDomain::project(rhs)))) {
        return false;
      }
      if (!PredicateRelationDomain::equal(
              PredicateRelationDomain::merge(lhs, rhs),
              PredicateRelationDomain::extend(
                  lhs, PredicateRelationDomain::project(rhs)))) {
        return false;
      }
    }
  }

  for (const auto &lhs : tensor) {
    for (const auto &rhs : tensor) {
      if (!PredicateTensorDomain::equal(
              PredicateTensorDomain::projectT(
                  PredicateTensorDomain::combine(lhs, rhs)),
              PredicateTensorDomain::combine(
                  PredicateTensorDomain::projectT(lhs),
                  PredicateTensorDomain::projectT(rhs)))) {
        return false;
      }
      const auto projected_lhs = PredicateTensorDomain::projectT(lhs);
      const auto projected_rhs = PredicateTensorDomain::projectT(rhs);
      if (!PredicateTensorDomain::equal(
              PredicateTensorDomain::extend(projected_lhs, projected_rhs),
              PredicateTensorDomain::projectT(
                  PredicateTensorDomain::extend(lhs, projected_rhs)))) {
        return false;
      }
      if (!PredicateTensorDomain::equal(
              PredicateTensorDomain::extend(projected_lhs, projected_rhs),
              PredicateTensorDomain::projectT(
                  PredicateTensorDomain::extend(projected_lhs, rhs)))) {
        return false;
      }
      if (!PredicateTensorDomain::equal(
              PredicateTensorDomain::merge(lhs, rhs),
              PredicateTensorDomain::extend(
                  lhs, PredicateTensorDomain::projectT(rhs)))) {
        return false;
      }
    }
  }

  return true;
}

} // namespace

PredicateRelation::Impl::~Impl() {
  if (bdd)
    Cudd_RecursiveDeref(getBaseManager(predicate_count), bdd);
}

PredicateTensorRelation::Impl::~Impl() {
  if (bdd)
    Cudd_RecursiveDeref(getTensorManager(predicate_count), bdd);
}

PredicateRelation::PredicateRelation()
    : impl(makeRelationImpl(
          activePredicateCount(),
          withRef(getBaseManager(activePredicateCount()), [&] {
            return logicZero(getBaseManager(activePredicateCount()));
          }))) {}

PredicateRelation::PredicateRelation(std::shared_ptr<Impl> impl_in)
    : impl(std::move(impl_in)) {}

PredicateRelation::PredicateRelation(const PredicateRelation &) = default;
PredicateRelation::PredicateRelation(PredicateRelation &&) noexcept = default;
PredicateRelation &
PredicateRelation::operator=(const PredicateRelation &) = default;
PredicateRelation &
PredicateRelation::operator=(PredicateRelation &&) noexcept = default;
PredicateRelation::~PredicateRelation() = default;

PredicateTensorRelation::PredicateTensorRelation()
    : impl(makeTensorImpl(
          activePredicateCount(),
          withRef(getTensorManager(activePredicateCount()), [&] {
            return logicZero(getTensorManager(activePredicateCount()));
          }))) {}

PredicateTensorRelation::PredicateTensorRelation(std::shared_ptr<Impl> impl_in)
    : impl(std::move(impl_in)) {}

PredicateTensorRelation::PredicateTensorRelation(
    const PredicateTensorRelation &) = default;
PredicateTensorRelation::PredicateTensorRelation(
    PredicateTensorRelation &&) noexcept = default;
PredicateTensorRelation &
PredicateTensorRelation::operator=(const PredicateTensorRelation &) = default;
PredicateTensorRelation &PredicateTensorRelation::operator=(
    PredicateTensorRelation &&) noexcept = default;
PredicateTensorRelation::~PredicateTensorRelation() = default;

void PredicateRelationDomain::configure(unsigned predicate_count,
                                        unsigned local_predicate_count) {
  assert(predicate_count > 0 &&
         "PredicateRelationDomain::configure requires at least one predicate");
  assert(local_predicate_count <= predicate_count);
  std::vector<unsigned> local_predicates;
  local_predicates.reserve(local_predicate_count);
  for (unsigned predicate = predicate_count - local_predicate_count;
       predicate < predicate_count; ++predicate) {
    local_predicates.push_back(predicate);
  }
  configure(predicate_count, local_predicates);
}

void PredicateRelationDomain::configure(
    unsigned predicate_count, const std::vector<unsigned> &local_predicates) {
  assert(predicate_count > 0 &&
         "PredicateRelationDomain::configure requires at least one predicate");
  configuredPredicateCountRef() = predicate_count;
  configuredLocalPredicateCountRef() =
      static_cast<unsigned>(local_predicates.size());
  auto &mask = configuredLocalPredicateMaskRef();
  mask.assign(predicate_count, false);
  for (unsigned predicate : local_predicates) {
    assert(predicate < predicate_count);
    assert(!mask[predicate] && "local predicate indices must be unique");
    mask[predicate] = true;
  }
}

bool PredicateRelationDomain::isConfigured() {
  return configuredPredicateCountRef() > 0;
}

unsigned PredicateRelationDomain::getPredicateCount() {
  return activePredicateCount();
}

unsigned PredicateRelationDomain::getLocalPredicateCount() {
  return activeLocalPredicateCount();
}

unsigned PredicateRelationDomain::getGlobalPredicateCount() {
  return activeGlobalPredicateCount();
}

bool PredicateRelationDomain::isLocalPredicate(unsigned predicate) {
  assert(predicate < activePredicateCount());
  return isConfiguredLocalPredicate(predicate);
}

PredicateRelationDomain::value_type PredicateRelationDomain::zero() {
  const unsigned predicate_count = activePredicateCount();
  return relationFromNode(predicate_count,
                          withRef(getBaseManager(predicate_count), [&] {
                            return logicZero(getBaseManager(predicate_count));
                          }));
}

PredicateRelationDomain::value_type PredicateRelationDomain::one() {
  return relationFromNode(activePredicateCount(),
                          relationIdentityNode(activePredicateCount()));
}

bool PredicateRelationDomain::equal(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  return implOf(a)->bdd == implOf(b)->bdd;
}

PredicateRelationDomain::value_type
PredicateRelationDomain::combine(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  const unsigned predicate_count = implOf(a)->predicate_count;
  return relationFromNode(
      predicate_count,
      bddOr(getBaseManager(predicate_count), implOf(a)->bdd, implOf(b)->bdd));
}

PredicateRelationDomain::value_type
PredicateRelationDomain::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::condCombine(bool phi, const value_type &t,
                                     const value_type &e) {
  return phi ? t : e;
}

PredicateRelationDomain::value_type
PredicateRelationDomain::extend(const value_type &outer,
                                const value_type &inner) {
  assert(implOf(outer)->predicate_count == implOf(inner)->predicate_count);
  const unsigned predicate_count = implOf(outer)->predicate_count;
  return relationFromNode(
      predicate_count,
      baseComposeNode(predicate_count, implOf(outer)->bdd, implOf(inner)->bdd));
}

PredicateRelationDomain::value_type
PredicateRelationDomain::extend_lin(const value_type &outer,
                                    const value_type &inner) {
  return extend(outer, inner);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::subtract(const value_type &a,
                                  const value_type & /*b*/) {
  return a;
}

PredicateRelationDomain::value_type
PredicateRelationDomain::assume(unsigned predicate, bool truthy) {
  const unsigned predicate_count = activePredicateCount();
  assert(predicate < predicate_count);
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = relationIdentityNode(predicate_count);
  DdNode *lit =
      truthy
          ? baseVarAt(predicate_count, BaseVarGroup::Cur, predicate)
          : Cudd_Not(baseVarAt(predicate_count, BaseVarGroup::Cur, predicate));
  DdNode *tmp = bddAnd(manager, node, lit);
  Cudd_RecursiveDeref(manager, node);
  return relationFromNode(predicate_count, tmp);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::assignConst(unsigned predicate, bool value) {
  const unsigned predicate_count = activePredicateCount();
  assert(predicate < predicate_count);
  DdManager *manager = getBaseManager(predicate_count);
  DdNode *node = logicOne(manager);
  Cudd_Ref(node);
  for (unsigned i = 0; i < predicate_count; ++i) {
    DdNode *constraint = nullptr;
    if (i == predicate) {
      constraint =
          value ? baseVarAt(predicate_count, BaseVarGroup::Next, i)
                : Cudd_Not(baseVarAt(predicate_count, BaseVarGroup::Next, i));
      Cudd_Ref(constraint);
    } else {
      constraint =
          bddXnor(manager, baseVarAt(predicate_count, BaseVarGroup::Cur, i),
                  baseVarAt(predicate_count, BaseVarGroup::Next, i));
    }
    DdNode *tmp = bddAnd(manager, node, constraint);
    Cudd_RecursiveDeref(manager, constraint);
    Cudd_RecursiveDeref(manager, node);
    node = tmp;
  }
  return relationFromNode(predicate_count, node);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::transpose(const value_type &relation) {
  return transposeRelationImpl(relation);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::project(const value_type &relation) {
  return projectRelationImpl(relation);
}

PredicateRelationDomain::value_type
PredicateRelationDomain::merge(const value_type &lhs, const value_type &rhs) {
  return extend(lhs, project(rhs));
}

PredicateRelationDomain::value_type PredicateRelationDomain::fromTransitions(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> &transitions) {
  return relationFromTransitionsImpl(activePredicateCount(), transitions);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>
PredicateRelationDomain::materialize(const value_type &relation) {
  return materializeRelationImpl(relation);
}

PredicateTensorDomain::value_type PredicateTensorDomain::zero() {
  const unsigned predicate_count = activePredicateCount();
  return tensorFromNode(predicate_count,
                        withRef(getTensorManager(predicate_count), [&] {
                          return logicZero(getTensorManager(predicate_count));
                        }));
}

PredicateTensorDomain::value_type PredicateTensorDomain::one() {
  return tensorFromNode(activePredicateCount(),
                        tensorIdentityNode(activePredicateCount()));
}

bool PredicateTensorDomain::equal(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  return implOf(a)->bdd == implOf(b)->bdd;
}

PredicateTensorDomain::value_type
PredicateTensorDomain::combine(const value_type &a, const value_type &b) {
  assert(implOf(a)->predicate_count == implOf(b)->predicate_count);
  const unsigned predicate_count = implOf(a)->predicate_count;
  return tensorFromNode(
      predicate_count,
      bddOr(getTensorManager(predicate_count), implOf(a)->bdd, implOf(b)->bdd));
}

PredicateTensorDomain::value_type
PredicateTensorDomain::ndetCombine(const value_type &a, const value_type &b) {
  return combine(a, b);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::condCombine(bool phi, const value_type &t,
                                   const value_type &e) {
  return phi ? t : e;
}

PredicateTensorDomain::value_type
PredicateTensorDomain::extend(const value_type &outer,
                              const value_type &inner) {
  assert(implOf(outer)->predicate_count == implOf(inner)->predicate_count);
  const unsigned predicate_count = implOf(outer)->predicate_count;
  return tensorFromNode(predicate_count,
                        tensorComposeNode(predicate_count, implOf(outer)->bdd,
                                          implOf(inner)->bdd));
}

PredicateTensorDomain::value_type
PredicateTensorDomain::extend_lin(const value_type &outer,
                                  const value_type &inner) {
  return extend(outer, inner);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::subtract(const value_type &a, const value_type & /*b*/) {
  return a;
}

PredicateTensorDomain::value_type
PredicateTensorDomain::couple(const PredicateRelation &lhs,
                              const PredicateRelation &rhs) {
  return coupleRelationImpl(lhs, rhs);
}

PredicateRelation PredicateTensorDomain::readout(const value_type &relation) {
  return readoutTensorImpl(relation);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::projectT(const value_type &relation) {
  return projectTensorImpl(relation);
}

PredicateTensorDomain::value_type
PredicateTensorDomain::merge(const value_type &lhs, const value_type &rhs) {
  return extend(lhs, projectT(rhs));
}

bool PredicateTensorDomain::validatePaperLaws() {
  if (!PredicateRelationDomain::isConfigured())
    return false;

  static std::mutex cache_mu;
  static std::unordered_map<std::string, bool> cache;

  const std::string key = currentPredicateConfigurationKey();
  {
    std::lock_guard<std::mutex> lock(cache_mu);
    const auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
  }

  const auto base_samples = validationBaseSamples();
  const auto tensor_samples = validationTensorSamples(base_samples);
  const bool valid =
      validatePredicateTensorCoreLaws(base_samples, tensor_samples) &&
      validatePredicateTensorProjectionLaws(base_samples, tensor_samples);

  {
    std::lock_guard<std::mutex> lock(cache_mu);
    cache[key] = valid;
  }
  return valid;
}

PredicateTensorDomain::value_type PredicateTensorDomain::fromTransitions(
    const std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t,
                                 std::uint64_t>> &transitions) {
  return tensorFromTransitionsImpl(activePredicateCount(), transitions);
}

std::vector<
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
PredicateTensorDomain::materialize(const value_type &relation) {
  return materializeTensorImpl(relation);
}

} // namespace npa
