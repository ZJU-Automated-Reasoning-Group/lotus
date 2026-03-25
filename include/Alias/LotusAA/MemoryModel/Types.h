/*
 * LotusAA - Type Definitions and Utilities
 * 
 * Common types, type aliases, and comparators used throughout LotusAA.
 * Provides LLVM-compatible data structures and helper types.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace llvm {

class PathCond {
public:
  struct Literal {
    enum class Kind {
      Value,
      Branch,
      SwitchCase,
      SwitchDefault,
      InvokeNormal,
      InvokeUnwind,
      Block,
      CallTarget,
      Imported,
      Opaque,
    };

    Kind kind;
    Value *value;
    BasicBlock *block;
    BasicBlock *successor;
    Function *callee;
    ConstantInt *case_value;
    const PathCond *opaque;

    Literal(Kind kind = Kind::Opaque, Value *value = nullptr,
            BasicBlock *block = nullptr, BasicBlock *successor = nullptr,
            Function *callee = nullptr, ConstantInt *case_value = nullptr,
            const PathCond *opaque = nullptr)
        : kind(kind), value(value), block(block), successor(successor),
          callee(callee), case_value(case_value), opaque(opaque) {}

    bool operator<(const Literal &other) const {
      return std::tie(kind, value, block, successor, callee, case_value,
                      opaque) <
             std::tie(other.kind, other.value, other.block, other.successor,
                      other.callee, other.case_value, other.opaque);
    }

    bool operator==(const Literal &other) const {
      return std::tie(kind, value, block, successor, callee, case_value,
                      opaque) ==
             std::tie(other.kind, other.value, other.block, other.successor,
                      other.callee, other.case_value, other.opaque);
    }
  };

  struct Cube {
    std::set<Literal> positive_literals;
    std::set<Literal> negative_literals;

    bool operator<(const Cube &other) const {
      return std::tie(positive_literals, negative_literals) <
             std::tie(other.positive_literals, other.negative_literals);
    }

    bool operator==(const Cube &other) const {
      return std::tie(positive_literals, negative_literals) ==
             std::tie(other.positive_literals, other.negative_literals);
    }

    bool empty() const {
      return positive_literals.empty() && negative_literals.empty();
    }
  };

  struct ConstraintSummary {
    bool always_true = false;
    bool always_false = true;
    std::set<Cube> cubes;

    bool operator<(const ConstraintSummary &other) const {
      return cubes < other.cubes;
    }
  };

  enum class Kind {
    True,
    False,
    ValueAtom,
    BranchAtom,
    SwitchCaseAtom,
    SwitchDefaultAtom,
    InvokeNormalAtom,
    InvokeUnwindAtom,
    BlockAtom,
    CallTargetAtom,
    ImportedAtom,
    Not,
    And,
    Or,
  };

private:
  Kind kind_;
  Value *value_;
  BasicBlock *block_;
  BasicBlock *successor_;
  Function *callee_;
  Function *owner_func_;
  ConstantInt *case_value_;
  PathCond *imported_;
  bool sense_;
  PathCond *lhs_;
  PathCond *rhs_;
  ConstraintSummary summary_;

  explicit PathCond(Kind kind)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), owner_func_(nullptr), case_value_(nullptr),
        imported_(nullptr),
        sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, bool sense)
      : kind_(Kind::ValueAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(nullptr), owner_func_(nullptr),
        case_value_(nullptr), imported_(nullptr), sense_(sense), lhs_(nullptr),
        rhs_(nullptr) {}

  PathCond(Kind kind, BasicBlock *block, BasicBlock *successor, Value *value,
           bool sense, Function *owner_func = nullptr,
           ConstantInt *case_value = nullptr, PathCond *imported = nullptr,
           Function *callee = nullptr)
      : kind_(kind), value_(value), block_(block), successor_(successor),
        callee_(callee), owner_func_(owner_func), case_value_(case_value),
        imported_(imported), sense_(sense), lhs_(nullptr), rhs_(nullptr) {}

  explicit PathCond(BasicBlock *block)
      : kind_(Kind::BlockAtom), value_(nullptr), block_(block),
        successor_(nullptr), callee_(nullptr),
        owner_func_(block ? block->getParent() : nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Value *value, Function *callee)
      : kind_(Kind::CallTargetAtom), value_(value), block_(nullptr),
        successor_(nullptr), callee_(callee), owner_func_(nullptr),
        case_value_(nullptr), imported_(nullptr), sense_(true),
        lhs_(nullptr), rhs_(nullptr) {}

  PathCond(Kind kind, PathCond *lhs, PathCond *rhs)
      : kind_(kind), value_(nullptr), block_(nullptr), successor_(nullptr),
        callee_(nullptr), owner_func_(nullptr), case_value_(nullptr),
        imported_(nullptr), sense_(true), lhs_(lhs), rhs_(rhs) {}

  static Function *inferOwner(Value *value) {
    if (auto *inst = dyn_cast_or_null<Instruction>(value))
      return inst->getFunction();
    if (auto *arg = dyn_cast_or_null<Argument>(value))
      return arg->getParent();
    return nullptr;
  }

  static Literal makeBooleanLiteral(Value *value) {
    return Literal(Literal::Kind::Value, value);
  }

  static ConstraintSummary makeFalseSummary() { return ConstraintSummary{}; }

  static ConstraintSummary makeTrueSummary() {
    ConstraintSummary summary;
    summary.cubes.insert(Cube{});
    summary.always_true = true;
    summary.always_false = false;
    return summary;
  }

  static void refreshSummaryFlags(ConstraintSummary &summary) {
    summary.always_false = summary.cubes.empty();
    summary.always_true =
        summary.cubes.size() == 1 && summary.cubes.begin()->empty();
  }

  static bool sameLiteralFamily(const Literal &lhs, const Literal &rhs) {
    switch (lhs.kind) {
    case Literal::Kind::Value:
      return rhs.kind == Literal::Kind::Value && lhs.value == rhs.value;
    case Literal::Kind::Branch:
      return rhs.kind == Literal::Kind::Branch && lhs.block == rhs.block;
    case Literal::Kind::SwitchCase:
    case Literal::Kind::SwitchDefault:
      return (rhs.kind == Literal::Kind::SwitchCase ||
              rhs.kind == Literal::Kind::SwitchDefault) &&
             lhs.block == rhs.block && lhs.value == rhs.value;
    case Literal::Kind::InvokeNormal:
    case Literal::Kind::InvokeUnwind:
      return (rhs.kind == Literal::Kind::InvokeNormal ||
              rhs.kind == Literal::Kind::InvokeUnwind) &&
             lhs.block == rhs.block;
    case Literal::Kind::Block:
      return rhs.kind == Literal::Kind::Block && lhs.block == rhs.block;
    case Literal::Kind::CallTarget:
      return rhs.kind == Literal::Kind::CallTarget && lhs.value == rhs.value;
    case Literal::Kind::Imported:
      return rhs.kind == Literal::Kind::Imported && lhs.opaque == rhs.opaque;
    case Literal::Kind::Opaque:
      return rhs.kind == Literal::Kind::Opaque && lhs.opaque == rhs.opaque;
    }
    return false;
  }

  static bool areMutuallyExclusive(const Literal &lhs, const Literal &rhs) {
    if (!sameLiteralFamily(lhs, rhs))
      return false;

    switch (lhs.kind) {
    case Literal::Kind::Value:
      return false;
    case Literal::Kind::Branch:
      return lhs.successor != rhs.successor;
    case Literal::Kind::SwitchCase:
      if (rhs.kind == Literal::Kind::SwitchDefault)
        return true;
      return lhs.case_value != rhs.case_value;
    case Literal::Kind::SwitchDefault:
      return rhs.kind == Literal::Kind::SwitchCase;
    case Literal::Kind::InvokeNormal:
      return rhs.kind == Literal::Kind::InvokeUnwind;
    case Literal::Kind::InvokeUnwind:
      return rhs.kind == Literal::Kind::InvokeNormal;
    case Literal::Kind::Block:
      return false;
    case Literal::Kind::CallTarget:
      return lhs.callee != rhs.callee;
    case Literal::Kind::Imported:
    case Literal::Kind::Opaque:
      return false;
    }
    return false;
  }

  static bool insertLiteral(Cube &cube, const Literal &literal, bool positive) {
    auto &dst = positive ? cube.positive_literals : cube.negative_literals;
    auto &other = positive ? cube.negative_literals : cube.positive_literals;

    if (other.count(literal))
      return false;

    if (positive) {
      for (const auto &existing : cube.positive_literals) {
        if (areMutuallyExclusive(existing, literal))
          return false;
      }
    }

    dst.insert(literal);
    return true;
  }

  static bool cubeSubsumes(const Cube &lhs, const Cube &rhs) {
    return std::includes(rhs.positive_literals.begin(), rhs.positive_literals.end(),
                         lhs.positive_literals.begin(),
                         lhs.positive_literals.end()) &&
           std::includes(rhs.negative_literals.begin(), rhs.negative_literals.end(),
                         lhs.negative_literals.begin(),
                         lhs.negative_literals.end());
  }

  static std::vector<std::pair<Literal, bool>>
  getSignedLiterals(const Cube &cube) {
    std::vector<std::pair<Literal, bool>> result;
    result.reserve(cube.positive_literals.size() + cube.negative_literals.size());
    for (const auto &lit : cube.positive_literals)
      result.emplace_back(lit, true);
    for (const auto &lit : cube.negative_literals)
      result.emplace_back(lit, false);
    return result;
  }

  static bool areComplementaryChoices(const Literal &lhs, bool lhs_positive,
                                      const Literal &rhs, bool rhs_positive) {
    if (sameLiteralFamily(lhs, rhs)) {
      if (lhs == rhs && lhs_positive != rhs_positive)
        return true;

      if (lhs_positive && rhs_positive) {
        switch (lhs.kind) {
        case Literal::Kind::Branch:
          return lhs.successor != rhs.successor;
        case Literal::Kind::InvokeNormal:
        case Literal::Kind::InvokeUnwind:
          return lhs.kind != rhs.kind;
        default:
          break;
        }
      }
    }

    return false;
  }

  static bool mergeComplementaryCubes(const Cube &lhs, const Cube &rhs,
                                      Cube &merged) {
    auto lhs_lits = getSignedLiterals(lhs);
    auto rhs_lits = getSignedLiterals(rhs);
    if (lhs_lits.size() != rhs_lits.size())
      return false;

    std::vector<std::pair<Literal, bool>> shared;
    std::vector<std::pair<Literal, bool>> lhs_only;
    std::vector<std::pair<Literal, bool>> rhs_only;

    for (const auto &lit : lhs_lits) {
      if (std::find(rhs_lits.begin(), rhs_lits.end(), lit) != rhs_lits.end()) {
        shared.push_back(lit);
      } else {
        lhs_only.push_back(lit);
      }
    }
    for (const auto &lit : rhs_lits) {
      if (std::find(lhs_lits.begin(), lhs_lits.end(), lit) == lhs_lits.end())
        rhs_only.push_back(lit);
    }

    if (lhs_only.size() != 1 || rhs_only.size() != 1)
      return false;

    if (!areComplementaryChoices(lhs_only.front().first, lhs_only.front().second,
                                 rhs_only.front().first, rhs_only.front().second))
      return false;

    Cube candidate;
    for (const auto &lit : shared) {
      if (!insertLiteral(candidate, lit.first, lit.second))
        return false;
    }
    merged = candidate;
    return true;
  }

  static void normalizeSummary(ConstraintSummary &summary) {
    if (summary.cubes.empty()) {
      summary = makeFalseSummary();
      return;
    }

    if (std::any_of(summary.cubes.begin(), summary.cubes.end(),
                    [](const Cube &cube) { return cube.empty(); })) {
      summary = makeTrueSummary();
      return;
    }

    bool changed = true;
    while (changed) {
      changed = false;

      for (auto it = summary.cubes.begin(); it != summary.cubes.end();) {
        bool subsumed = false;
        for (auto other = summary.cubes.begin(); other != summary.cubes.end();
             ++other) {
          if (it == other)
            continue;
          if (cubeSubsumes(*other, *it)) {
            subsumed = true;
            break;
          }
        }
        if (subsumed) {
          it = summary.cubes.erase(it);
          changed = true;
        } else {
          ++it;
        }
      }

      if (summary.cubes.empty()) {
        summary = makeFalseSummary();
        return;
      }

      bool merged_any = false;
      for (auto lhs = summary.cubes.begin(); lhs != summary.cubes.end(); ++lhs) {
        auto rhs = lhs;
        ++rhs;
        for (; rhs != summary.cubes.end(); ++rhs) {
          Cube merged;
          if (!mergeComplementaryCubes(*lhs, *rhs, merged))
            continue;

          Cube lhs_cube = *lhs;
          Cube rhs_cube = *rhs;
          summary.cubes.erase(lhs_cube);
          summary.cubes.erase(rhs_cube);
          summary.cubes.insert(merged);
          changed = true;
          merged_any = true;
          break;
        }
        if (merged_any)
          break;
      }

      if (merged_any &&
          std::any_of(summary.cubes.begin(), summary.cubes.end(),
                      [](const Cube &cube) { return cube.empty(); })) {
        summary = makeTrueSummary();
        return;
      }
    }

    refreshSummaryFlags(summary);
  }

  static ConstraintSummary makeAtomicSummary(const Literal &literal,
                                             bool positive) {
    ConstraintSummary summary;
    Cube cube;
    if (!insertLiteral(cube, literal, positive))
      return makeFalseSummary();
    summary.cubes.insert(cube);
    normalizeSummary(summary);
    return summary;
  }

  static ConstraintSummary orSummaries(const ConstraintSummary &lhs,
                                       const ConstraintSummary &rhs) {
    if (lhs.always_true || rhs.always_true)
      return makeTrueSummary();
    if (lhs.always_false)
      return rhs;
    if (rhs.always_false)
      return lhs;

    ConstraintSummary result;
    result.cubes = lhs.cubes;
    result.cubes.insert(rhs.cubes.begin(), rhs.cubes.end());
    normalizeSummary(result);
    return result;
  }

  static ConstraintSummary andSummaries(const ConstraintSummary &lhs,
                                        const ConstraintSummary &rhs) {
    if (lhs.always_false || rhs.always_false)
      return makeFalseSummary();
    if (lhs.always_true)
      return rhs;
    if (rhs.always_true)
      return lhs;

    ConstraintSummary result;
    for (const auto &lhs_cube : lhs.cubes) {
      for (const auto &rhs_cube : rhs.cubes) {
        Cube merged = lhs_cube;
        bool ok = true;
        for (const auto &lit : rhs_cube.positive_literals) {
          if (!insertLiteral(merged, lit, true)) {
            ok = false;
            break;
          }
        }
        if (!ok)
          continue;
        for (const auto &lit : rhs_cube.negative_literals) {
          if (!insertLiteral(merged, lit, false)) {
            ok = false;
            break;
          }
        }
        if (ok)
          result.cubes.insert(merged);
      }
    }
    normalizeSummary(result);
    return result;
  }

  static std::vector<Cube> negatePositiveLiteral(const Literal &literal) {
    std::vector<Cube> result;
    switch (literal.kind) {
    case Literal::Kind::Value: {
      Cube cube;
      insertLiteral(cube, literal, false);
      result.push_back(cube);
      break;
    }
    case Literal::Kind::Branch: {
      auto *br = literal.block ? dyn_cast_or_null<BranchInst>(
                                     literal.block->getTerminator())
                               : nullptr;
      if (br && br->isConditional() && br->getNumSuccessors() == 2) {
        Cube cube;
        Literal other = literal;
        other.successor = literal.successor == br->getSuccessor(0)
                              ? br->getSuccessor(1)
                              : br->getSuccessor(0);
        insertLiteral(cube, other, true);
        result.push_back(cube);
      } else {
        Cube cube;
        insertLiteral(cube, literal, false);
        result.push_back(cube);
      }
      break;
    }
    case Literal::Kind::SwitchCase:
    case Literal::Kind::SwitchDefault: {
      auto *sw = literal.block ? dyn_cast_or_null<SwitchInst>(
                                     literal.block->getTerminator())
                               : nullptr;
      if (!sw) {
        Cube cube;
        insertLiteral(cube, literal, false);
        result.push_back(cube);
        break;
      }

      for (const auto &case_it : sw->cases()) {
        if (literal.kind == Literal::Kind::SwitchCase &&
            case_it.getCaseValue() == literal.case_value)
          continue;
        Cube cube;
        Literal case_lit(Literal::Kind::SwitchCase, sw->getCondition(),
                         literal.block, case_it.getCaseSuccessor(), nullptr,
                         case_it.getCaseValue());
        insertLiteral(cube, case_lit, true);
        result.push_back(cube);
      }

      if (literal.kind != Literal::Kind::SwitchDefault) {
        Cube cube;
        Literal default_lit(Literal::Kind::SwitchDefault, sw->getCondition(),
                            literal.block, sw->getDefaultDest());
        insertLiteral(cube, default_lit, true);
        result.push_back(cube);
      }

      if (result.empty()) {
        Cube cube;
        insertLiteral(cube, literal, false);
        result.push_back(cube);
      }
      break;
    }
    case Literal::Kind::InvokeNormal:
    case Literal::Kind::InvokeUnwind: {
      Cube cube;
      Literal other = literal;
      other.kind = literal.kind == Literal::Kind::InvokeNormal
                       ? Literal::Kind::InvokeUnwind
                       : Literal::Kind::InvokeNormal;
      insertLiteral(cube, other, true);
      result.push_back(cube);
      break;
    }
    case Literal::Kind::Block:
    case Literal::Kind::CallTarget:
    case Literal::Kind::Imported:
    case Literal::Kind::Opaque: {
      Cube cube;
      insertLiteral(cube, literal, false);
      result.push_back(cube);
      break;
    }
    }
    return result;
  }

  static ConstraintSummary negateSummary(const ConstraintSummary &input) {
    if (input.always_true)
      return makeFalseSummary();
    if (input.always_false)
      return makeTrueSummary();

    ConstraintSummary result = makeTrueSummary();
    for (const auto &cube : input.cubes) {
      ConstraintSummary negated_cube = makeFalseSummary();

      for (const auto &lit : cube.positive_literals) {
        ConstraintSummary lit_summary;
        for (const auto &neg_cube : negatePositiveLiteral(lit))
          lit_summary.cubes.insert(neg_cube);
        normalizeSummary(lit_summary);
        negated_cube = orSummaries(negated_cube, lit_summary);
      }

      for (const auto &lit : cube.negative_literals) {
        negated_cube =
            orSummaries(negated_cube, makeAtomicSummary(lit, true));
      }

      result = andSummaries(result, negated_cube);
    }

    normalizeSummary(result);
    return result;
  }

  void initializeAtomicSummary() {
    switch (kind_) {
    case Kind::True:
      summary_ = makeTrueSummary();
      break;
    case Kind::False:
      summary_ = makeFalseSummary();
      break;
    case Kind::ValueAtom:
      summary_ = makeAtomicSummary(makeBooleanLiteral(value_), sense_);
      break;
    case Kind::BranchAtom:
      summary_ = makeAtomicSummary(makeBooleanLiteral(value_), sense_);
      break;
    case Kind::SwitchCaseAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::SwitchCase, value_, block_, successor_,
                  nullptr, case_value_),
          true);
      break;
    case Kind::SwitchDefaultAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::SwitchDefault, value_, block_, successor_),
          true);
      break;
    case Kind::InvokeNormalAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::InvokeNormal, nullptr, block_, successor_),
          true);
      break;
    case Kind::InvokeUnwindAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::InvokeUnwind, nullptr, block_, successor_),
          true);
      break;
    case Kind::BlockAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::Block, nullptr, block_), true);
      break;
    case Kind::CallTargetAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::CallTarget, value_, nullptr, nullptr, callee_),
          true);
      break;
    case Kind::ImportedAtom:
      summary_ = makeAtomicSummary(
          Literal(Literal::Kind::Imported, nullptr, nullptr, nullptr, nullptr,
                  nullptr, imported_),
          true);
      break;
    case Kind::Not:
    case Kind::And:
    case Kind::Or:
      break;
    }
  }

  void initializeNotSummary() {
    if (!lhs_)
      return;
    summary_ = negateSummary(lhs_->getConstraintSummary());
  }

  void initializeAndSummary() {
    if (!lhs_ || !rhs_)
      return;
    summary_ = andSummaries(lhs_->getConstraintSummary(),
                            rhs_->getConstraintSummary());
  }

  void initializeOrSummary() {
    if (!lhs_ || !rhs_)
      return;
    summary_ = orSummaries(lhs_->getConstraintSummary(),
                           rhs_->getConstraintSummary());
  }

public:
  static PathCond *createTrue() {
    PathCond *cond = new PathCond(Kind::True);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createFalse() {
    PathCond *cond = new PathCond(Kind::False);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createValueAtom(Value *value, bool sense) {
    PathCond *cond = new PathCond(value, sense);
    cond->owner_func_ = inferOwner(value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createBranchAtom(BasicBlock *block, BasicBlock *successor,
                                    Value *value, bool sense) {
    PathCond *cond =
        new PathCond(Kind::BranchAtom, block, successor, value, sense,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createSwitchCaseAtom(BasicBlock *block, BasicBlock *successor,
                                        Value *value, ConstantInt *case_value) {
    PathCond *cond =
        new PathCond(Kind::SwitchCaseAtom, block, successor, value, true,
                     block ? block->getParent() : nullptr, case_value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createSwitchDefaultAtom(BasicBlock *block,
                                           BasicBlock *successor,
                                           Value *value) {
    PathCond *cond =
        new PathCond(Kind::SwitchDefaultAtom, block, successor, value, true,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createInvokeNormalAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    PathCond *cond =
        new PathCond(Kind::InvokeNormalAtom, block, successor, nullptr, true,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createInvokeUnwindAtom(BasicBlock *block,
                                          BasicBlock *successor) {
    PathCond *cond =
        new PathCond(Kind::InvokeUnwindAtom, block, successor, nullptr, false,
                     block ? block->getParent() : nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createBlockAtom(BasicBlock *block) {
    PathCond *cond = new PathCond(block);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createCallTargetAtom(Value *value, Function *callee) {
    PathCond *cond = new PathCond(value, callee);
    cond->owner_func_ = inferOwner(value);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createImportedAtom(Function *owner_func, PathCond *imported) {
    PathCond *cond =
        new PathCond(Kind::ImportedAtom, nullptr, nullptr, nullptr, true,
                     owner_func, nullptr, imported, nullptr);
    cond->initializeAtomicSummary();
    return cond;
  }
  static PathCond *createNot(PathCond *inner) {
    PathCond *cond = new PathCond(Kind::Not, inner, nullptr);
    cond->owner_func_ = inner ? inner->getOwnerFunc() : nullptr;
    cond->initializeNotSummary();
    return cond;
  }
  static PathCond *createAnd(PathCond *lhs, PathCond *rhs) {
    PathCond *cond = new PathCond(Kind::And, lhs, rhs);
    if (lhs && rhs && lhs->getOwnerFunc() == rhs->getOwnerFunc())
      cond->owner_func_ = lhs->getOwnerFunc();
    cond->initializeAndSummary();
    return cond;
  }
  static PathCond *createOr(PathCond *lhs, PathCond *rhs) {
    PathCond *cond = new PathCond(Kind::Or, lhs, rhs);
    if (lhs && rhs && lhs->getOwnerFunc() == rhs->getOwnerFunc())
      cond->owner_func_ = lhs->getOwnerFunc();
    cond->initializeOrSummary();
    return cond;
  }

  Kind getKind() const { return kind_; }
  Value *getValue() const { return value_; }
  BasicBlock *getBlock() const { return block_; }
  BasicBlock *getSuccessor() const { return successor_; }
  Function *getCallee() const { return callee_; }
  Function *getOwnerFunc() const { return owner_func_; }
  ConstantInt *getCaseValue() const { return case_value_; }
  PathCond *getImportedSource() const { return imported_; }
  bool getSense() const { return sense_; }
  PathCond *getLhs() const { return lhs_; }
  PathCond *getRhs() const { return rhs_; }
  const ConstraintSummary &getConstraintSummary() const { return summary_; }
  bool isCompound() const {
    return kind_ == Kind::Not || kind_ == Kind::And || kind_ == Kind::Or;
  }

  void print(raw_ostream &OS) const {
    switch (kind_) {
    case Kind::True:
      OS << "true";
      break;
    case Kind::False:
      OS << "false";
      break;
    case Kind::ValueAtom:
      if (!sense_)
        OS << "!";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "<null-cond>";
      }
      break;
    case Kind::BranchAtom:
      OS << "branch(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ", ";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << ", " << (sense_ ? "true" : "false") << ")";
      break;
    case Kind::SwitchCaseAtom:
      OS << "switch-case(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ", ";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " == ";
      if (case_value_)
        case_value_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::SwitchDefaultAtom:
      OS << "switch-default(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::InvokeNormalAtom:
    case Kind::InvokeUnwindAtom:
      OS << (kind_ == Kind::InvokeNormalAtom ? "invoke-normal("
                                             : "invoke-unwind(");
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << " -> ";
      if (successor_)
        OS << successor_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::BlockAtom:
      OS << "bb(";
      if (block_)
        OS << block_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::CallTargetAtom:
      OS << "calltarget(";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " == ";
      if (callee_)
        OS << callee_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::ImportedAtom:
      OS << "imported(";
      if (value_) {
        if (value_->hasName())
          OS << value_->getName();
        else
          value_->print(OS);
      } else {
        OS << "null";
      }
      OS << " :: ";
      if (callee_)
        OS << callee_->getName();
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::Not:
      OS << "!(";
      if (lhs_)
        lhs_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    case Kind::And:
    case Kind::Or:
      OS << "(";
      if (lhs_)
        lhs_->print(OS);
      else
        OS << "null";
      OS << (kind_ == Kind::And ? " && " : " || ");
      if (rhs_)
        rhs_->print(OS);
      else
        OS << "null";
      OS << ")";
      break;
    }
  }
};

using path_cond_t = PathCond *;

// LLVM value comparator for map/set ordering
struct llvm_cmp {
  bool operator()(const Value *A, const Value *B) const {
    return A < B;
  }
  
  bool operator()(const BasicBlock *A, const BasicBlock *B) const {
    return A < B;
  }
  
  bool operator()(const Function *A, const Function *B) const {
    return A < B;
  }
};

// Singleton for consistent value indexing
class LLVMValueIndex {
  static LLVMValueIndex *Instance;
  LLVMValueIndex() {}
  
public:
  static LLVMValueIndex *get() {
    if (!Instance)
      Instance = new LLVMValueIndex();
    return Instance;
  }
};

} // namespace llvm
