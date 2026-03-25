/**
 * @file ParamStrategy.h
 * @brief Parameterization strategies and helpers for instantiating
 * SymAbsAI abstract domains over sets or pairs of LLVM values.
 */
#pragma once

#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Domains/Product.h"

#include <z3++.h>

namespace symabs_ai {
/**
 * Represents a strategy used to determine expression parameters of
 * parameterized domains.
 *
 * A parametrization strategy consists of an arity and a function that
 * produces a vector of expression tuples of its arity. When the strategy
 * is applied using `DomainConstructor::parameterize`, the arity of the
 * domain constructor is decreased by the arity of the `ParamStrategy` and
 * some of the constructor's arguments are fixed using the expressions
 * from the strategy object. Multiple expression tuples produced by
 * `ParamStrategy` are combined using reduced product.
 *
 * Unlike domain constructors, parameterization strategies do not have an
 * associated registry and are all implemented as static factory methods of
 * this class. To add a new one, you will also need to modify the code in
 * `python.cpp` to expose the factory method to the Python configuration API.
 */
class ParamStrategy {
public:
  typedef std::vector<llvm::SmallVector<Expression, 2>> params_t;

private:
  int Arity_;
  std::function<params_t(const DomainConstructor::args &)> ParamsFunc_;

public:
  ParamStrategy(
      int arity,
      std::function<params_t(const DomainConstructor::args &)> params_func)
      : Arity_(arity), ParamsFunc_(std::move(params_func)) {}

  params_t generateParams(const DomainConstructor::args &args) const {
    params_t result = ParamsFunc_(args);
#ifndef NDEBUG
    for (auto &p : result) {
      assert((int)p.size() == Arity_);
    }
#endif
    return result;
  }

  int arity() const { return Arity_; }

  static ParamStrategy FromExpression(Expression e);
  static ParamStrategy AllValues();
  static ParamStrategy AllValuePairs(bool symmetric = false);
  static ParamStrategy PackedPairs(bool symmetric = false);
  static ParamStrategy ConstOffsets(bool symmetric = false);
  static ParamStrategy PointerPairs(bool symmetric = false);
  static ParamStrategy NonPointerPairs(bool symmetric = false);
  static ParamStrategy Pointers();
  static ParamStrategy NonPointers();

  friend std::ostream &operator<<(std::ostream &out, const ParamStrategy &ps) {
    return out << "<ParamStrategy arity=" << ps.arity() << ">";
  }
};

namespace params {
using symabs_ai::domains::Product;

inline bool hasCompatibleType(const FunctionContext &fctx, llvm::Value *a,
                              llvm::Value *b) {
  z3::sort sort_a = fctx.sortForType(a->getType());
  z3::sort sort_b = fctx.sortForType(b->getType());

  if (Z3_sort(sort_a) == Z3_sort(sort_b))
    return true;

  unsigned int ptr_bits = fctx.getPointerSize();

  if (a->getType()->isPointerTy() && sort_b.is_bv() &&
      sort_b.bv_size() == ptr_bits)
    return true;

  if (b->getType()->isPointerTy() && sort_a.is_bv() &&
      sort_a.bv_size() == ptr_bits)
    return true;

  return false;
}

template <typename T>
unique_ptr<AbstractValue> ForValues(const FunctionContext &fctx,
                                    llvm::BasicBlock *bb, bool after) {
  Product *result = new Product(fctx);

  for (auto value : fctx.valuesAvailableIn(bb, after)) {
    unique_ptr<AbstractValue> ptr(new T(fctx, value));
    result->add(std::move(ptr));
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

template <typename T>
unique_ptr<AbstractValue> ForNonPointers(const FunctionContext &fctx,
                                         llvm::BasicBlock *bb, bool after) {
  Product *result = new Product(fctx);

  for (auto value : fctx.valuesAvailableIn(bb, after)) {
    if (value->getType()->isPointerTy())
      continue;

    unique_ptr<AbstractValue> ptr(new T(fctx, value));
    result->add(std::move(ptr));
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

template <typename T>
unique_ptr<AbstractValue> ForPointers(const FunctionContext &fctx,
                                      llvm::BasicBlock *bb, bool after) {
  Product *result = new Product(fctx);

  for (auto value : fctx.valuesAvailableIn(bb, after)) {
    if (!value->getType()->isPointerTy())
      continue;

    unique_ptr<AbstractValue> ptr(new T(fctx, value));
    result->add(std::move(ptr));
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

/**
 *  If `symmetric` is `false`, returns a Product containing an
 *  AbstractValue for every pair of available Values in `bb`.
 *
 *  If `symmetric` is `true`, returns a Product containing exactly one
 *  AbstractValue for every combination of different Values available in
 *  `bb` (i.e. no two AbstractValues with only swapped Values).
 */
template <typename T>
unique_ptr<AbstractValue> ForValuePairs(const FunctionContext &fctx,
                                        llvm::BasicBlock *bb, bool after,
                                        bool symmetric = false) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);

  for (int i = 0; i < (int)vars.size(); i++) {
    int j = symmetric ? i + 1 : 0;
    for (; j < (int)vars.size(); j++) {
      if (i != j && hasCompatibleType(fctx, vars[i], vars[j])) {
        T *ptr = new T(fctx, vars[i], vars[j]);
        result->add(unique_ptr<AbstractValue>(ptr));
      }
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

template <typename T>
unique_ptr<AbstractValue> ForPointerPairs(const FunctionContext &fctx,
                                          llvm::BasicBlock *bb, bool after,
                                          bool symmetric = false) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);

  for (int i = 0; i < (int)vars.size(); i++) {
    if (!vars[i]->getType()->isPointerTy())
      continue;
    int j = symmetric ? i + 1 : 0;
    for (; j < (int)vars.size(); j++) {
      if (!vars[j]->getType()->isPointerTy())
        continue;
      if (i != j && hasCompatibleType(fctx, vars[i], vars[j])) {
        T *ptr = new T(fctx, vars[i], vars[j]);
        result->add(unique_ptr<AbstractValue>(ptr));
      }
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

template <typename T>
unique_ptr<AbstractValue> ForNonPointerPairs(const FunctionContext &fctx,
                                             llvm::BasicBlock *bb, bool after,
                                             bool symmetric = false) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);

  for (int i = 0; i < (int)vars.size(); i++) {
    if (vars[i]->getType()->isPointerTy())
      continue;
    int j = symmetric ? i + 1 : 0;
    for (; j < (int)vars.size(); j++) {
      if (vars[j]->getType()->isPointerTy())
        continue;
      if (i != j && hasCompatibleType(fctx, vars[i], vars[j])) {
        T *ptr = new T(fctx, vars[i], vars[j]);
        result->add(unique_ptr<AbstractValue>(ptr));
      }
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

/**
 *  Returns a `Product` of AbstractValues for all available represented
 *  Values in the specified BasicBlock restricted to the following:
 *
 *  For every pair of represented Values with identical bitwidths `a` and `b`
 *  with `a != b` where `a` is used inside `bb` and `b` is available at the
 *  first use of `a` in `bb`, the returned product contains exactly one
 *  AbstractValue with these two Values.
 */
template <typename T>
unique_ptr<AbstractValue> ForValuePairsRestricted(const FunctionContext &fctx,
                                                  llvm::BasicBlock *bb,
                                                  bool after) {
  Product *result = new Product(fctx);

  if (!bb) { // Return empty Product for Fragment::Exit
    result->finalize();
    return unique_ptr<AbstractValue>(result);
  }

  auto vars_avail = fctx.valuesAvailableIn(bb, after);
  std::set<llvm::Value *> defined_before;
  std::set<std::pair<llvm::Value *, llvm::Value *>> seen;

  // using a lambda here to avoid redundant code and to exploit its
  // closure properties
  auto addToResult = [&](llvm::Value *current) {
    if (!fctx.isRepresentedValue(current))
      return;
    for (unsigned int i = 0; i < vars_avail.size(); i++) {
      if (current == vars_avail[i])
        continue; // identical operands are not of interest
      if (current->getType() != vars_avail[i]->getType())
        continue; // only consider values with the same types
      if ((seen.find({current, vars_avail[i]}) != seen.end()) ||
          (seen.find({vars_avail[i], current}) != seen.end()))
        continue; // avoid duplicate absvals with swapped args
      if (auto *avail_inst = llvm::dyn_cast_or_null<llvm::Instruction>(
              (llvm::Value *)vars_avail[i])) {

        if ((avail_inst->getParent() == bb) &&
            (defined_before.find(avail_inst) == defined_before.end())) {
          continue;
          // vars_avail[i] is an instruction inside bb that is not
          // defined before current and can therefore not be a
          // replacement for current here.
        }
      }
      seen.insert({current, vars_avail[i]});
      T *ptr = new T(fctx, *fctx.findRepresentedValue(current), vars_avail[i]);
      result->add(unique_ptr<AbstractValue>(ptr));
    }
  };

  // iterate over all in non-PHIs used values inside bb
  for (auto &instr : *bb) {
    if (llvm::isa<llvm::PHINode>(instr))
      continue; // PHINode Operands are handled in the predecessors
    for (unsigned int i = 0; i < instr.getNumOperands(); i++) {
      auto *current = instr.getOperand(i);
      addToResult(current);
    }
    defined_before.insert(&instr);
  }
  // Add the Values that are used in the PHINodes of the successor bbs
  for (auto itr_bb_to = succ_begin(bb), end = succ_end(bb); itr_bb_to != end;
       ++itr_bb_to) {
    for (auto &instr : **itr_bb_to) {
      if (!llvm::isa<llvm::PHINode>(instr))
        break;
      auto &phi = llvm::cast<llvm::PHINode>(instr);
      auto *current = phi.getIncomingValueForBlock(bb);
      addToResult(current);
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

bool isInPack(FragmentDecomposition &decomp, llvm::Value *a, llvm::Value *b);

/**
 *  Returns a Product of AbstractValues for all available represented Values
 *  in the specified BasicBlock that are used or defined in the same fragment
 *  according to the given FragmentDecomposition.
 */
template <typename T>
unique_ptr<AbstractValue>
ForFragments(const FunctionContext &fctx, llvm::BasicBlock *bb, bool after,
             FragmentDecomposition &decomp, bool symmetric = false) {
  Product *result = new Product(fctx);
  auto vars = fctx.valuesAvailableIn(bb, after);

  for (int i = 0; i < (int)vars.size(); i++) {
    int j = symmetric ? i + 1 : 0;
    for (; j < (int)vars.size(); j++) {
      if (!isInPack(decomp, vars[i], vars[j]))
        continue;
      if (i != j && hasCompatibleType(fctx, vars[i], vars[j])) {
        T *ptr = new T(fctx, vars[i], vars[j]);
        result->add(unique_ptr<AbstractValue>(ptr));
      }
    }
  }

  result->finalize();
  return unique_ptr<AbstractValue>(result);
}

} // namespace params
} // namespace symabs_ai
