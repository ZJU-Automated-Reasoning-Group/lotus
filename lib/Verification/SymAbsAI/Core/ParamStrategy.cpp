#include "Verification/SymAbsAI/Core/ParamStrategy.h"

#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

namespace symabs_ai {
ParamStrategy ParamStrategy::FromExpression(Expression e) {
  auto f = [e](const DomainConstructor::args &args) {
    (void)args; // `args` is unused but kept for a uniform signature
    params_t result;
    llvm::SmallVector<Expression, 2> p;
    p.push_back(e);
    result.push_back(p);
    return result;
  };

  return ParamStrategy(1, f);
}

ParamStrategy ParamStrategy::AllValues() {
  // Instantiate a unary domain once for every value available at the
  // program point. This is typically used for simple per-variable
  // domains such as constant propagation.
  auto f = [](const DomainConstructor::args &args) {
    params_t result;
    auto vars = args.fctx->valuesAvailableIn(args.location, args.is_after_bb);

    for (auto &value : vars) {
      llvm::SmallVector<Expression, 2> p;
      p.push_back(value);
      result.push_back(p);
    }

    return result;
  };

  return ParamStrategy(1, f);
}

ParamStrategy ParamStrategy::AllValuePairs(bool symmetric) {
  // Instantiate a binary domain for every pair of values that share
  // the same SMT sort (optionally only once per unordered pair when
  // `symmetric` is true). Used for generic relational domains.
  auto f = [symmetric](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto vars = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (int i = 0; i < (int)vars.size(); i++) {
      int j = symmetric ? i + 1 : 0;
      for (; j < (int)vars.size(); j++) {
        z3::sort sort_i = fctx.sortForType(vars[i]->getType());
        z3::sort sort_j = fctx.sortForType(vars[j]->getType());

        if (i != j && ((Z3_sort)sort_i == sort_j)) {
          llvm::SmallVector<Expression, 2> p;
          p.push_back(vars[i]);
          p.push_back(vars[j]);
          result.push_back(p);
        }
      }
    }

    return result;
  };

  return ParamStrategy(2, f);
}

ParamStrategy ParamStrategy::PackedPairs(bool symmetric) {
  // Like `AllValuePairs` but restrict pairs to values that are used
  // or defined within the same fragment (a “pack”) according to a
  // loop-header-based fragment decomposition. This can significantly
  // reduce the number of relational domains.
  auto f = [symmetric](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto vars = fctx.valuesAvailableIn(args.location, args.is_after_bb);
    auto decomp = FragmentDecomposition::For(
        fctx, FragmentDecomposition::strategy::Headers);

    for (int i = 0; i < (int)vars.size(); i++) {
      int j = symmetric ? i + 1 : 0;
      for (; j < (int)vars.size(); j++) {
        if (!params::isInPack(decomp, vars[i], vars[j]))
          continue;
        z3::sort sort_i = fctx.sortForType(vars[i]->getType());
        z3::sort sort_j = fctx.sortForType(vars[j]->getType());

        if (i != j && ((Z3_sort)sort_i == sort_j)) {
          llvm::SmallVector<Expression, 2> p;
          p.push_back(vars[i]);
          p.push_back(vars[j]);
          result.push_back(p);
        }
      }
    }

    return result;
  };

  return ParamStrategy(2, f);
}

ParamStrategy ParamStrategy::ConstOffsets(bool symmetric) {
  // Instantiate a binary domain for expressions that differ only by
  // a constant offset (e.g. x + c, x - c, or constant GEPs). This is
  // useful for numeric or pointer-offset relations and can optionally
  // generate both directions when `symmetric` is false.
  auto f = [symmetric](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto vars = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (int i = 0; i < (int)vars.size(); i++) {
      llvm::Value *var = nullptr;
      z3::expr offset = fctx.getZ3().bool_val(false);
      bool sub = false;

      if (auto *bin_op = llvm::dyn_cast<llvm::BinaryOperator>(&*vars[i])) {
        if (bin_op->getOpcode() == llvm::Instruction::Add) {
          sub = false;
        } else if (bin_op->getOpcode() == llvm::Instruction::Sub) {
          sub = true;
        } else {
          continue;
        }

        if (fctx.isRepresentedValue(bin_op->getOperand(0))) {
          var = bin_op->getOperand(0);
        } else if (fctx.isRepresentedValue(bin_op->getOperand(1))) {
          var = bin_op->getOperand(1);
        } else {
          continue;
        }

        llvm::ConstantInt *constant = nullptr;
        if (auto *c =
                llvm::dyn_cast<llvm::ConstantInt>(bin_op->getOperand(0))) {
          constant = c;
        } else if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(
                       bin_op->getOperand(1))) {
          constant = c;
        } else {
          continue;
        }
        offset = makeConstantInt(&fctx.getZ3(), constant);

      } else if (auto *gep =
                     llvm::dyn_cast<llvm::GetElementPtrInst>(&*vars[i])) {
        var = gep->getPointerOperand();
        if (!fctx.isRepresentedValue(var))
          continue;

        unsigned bits = 0;
        if (var->getType()->isIntegerTy())
          bits = var->getType()->getIntegerBitWidth();
        else
          bits = fctx.getPointerSize();

        llvm::DataLayout DL(fctx.getFunction()->getParent());
        llvm::APInt gep_offset(bits, 0);
        bool succ = gep->accumulateConstantOffset(DL, gep_offset);

        if (!succ)
          continue;

        offset =
            fctx.getZ3().bv_val((uint64_t)gep_offset.getLimitedValue(), bits);

      } else {
        continue;
      }
      Expression other =
          sub ? Expression(*fctx.findRepresentedValue(var)) - Expression(offset)
              : Expression(*fctx.findRepresentedValue(var)) +
                    Expression(offset);

      llvm::SmallVector<Expression, 2> p;
      p.push_back(vars[i]);
      p.push_back(other);
      result.push_back(p);

      if (!symmetric) {
        llvm::SmallVector<Expression, 2> p;
        p.push_back(other);
        p.push_back(vars[i]);
        result.push_back(p);
      }
    }

    return result;
  };

  return ParamStrategy(2, f);
}

ParamStrategy ParamStrategy::PointerPairs(bool symmetric) {
  // Instantiate a binary domain for pairs of pointer-typed values
  // with compatible SMT sorts (again optionally symmetric). Intended
  // for pointer (dis)aliasing or offset relations.
  auto f = [symmetric](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto vars = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (int i = 0; i < (int)vars.size(); i++) {
      if (!vars[i]->getType()->isPointerTy())
        continue;
      int j = symmetric ? i + 1 : 0;
      for (; j < (int)vars.size(); j++) {
        if (!vars[j]->getType()->isPointerTy())
          continue;

        if (i != j && params::hasCompatibleType(fctx, vars[i], vars[j])) {
          llvm::SmallVector<Expression, 2> p;
          p.push_back(vars[i]);
          p.push_back(vars[j]);
          result.push_back(p);
        }
      }
    }

    return result;
  };

  return ParamStrategy(2, f);
}

ParamStrategy ParamStrategy::NonPointerPairs(bool symmetric) {
  // Instantiate a binary domain for pairs of non-pointer values with
  // compatible SMT sorts. This is often used for purely numeric
  // relational domains.
  auto f = [symmetric](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto vars = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (int i = 0; i < (int)vars.size(); i++) {
      if (vars[i]->getType()->isPointerTy())
        continue;
      int j = symmetric ? i + 1 : 0;
      for (; j < (int)vars.size(); j++) {
        if (vars[j]->getType()->isPointerTy())
          continue;

        if (i != j && params::hasCompatibleType(fctx, vars[i], vars[j])) {
          llvm::SmallVector<Expression, 2> p;
          p.push_back(vars[i]);
          p.push_back(vars[j]);
          result.push_back(p);
        }
      }
    }

    return result;
  };

  return ParamStrategy(2, f);
}

ParamStrategy ParamStrategy::Pointers() {
  auto f = [](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto values = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (auto value : values) {
      if (!value->getType()->isPointerTy())
        continue;

      llvm::SmallVector<Expression, 2> p;
      p.push_back(value);
      result.push_back(p);
    }

    return result;
  };

  return ParamStrategy(1, f);
}

ParamStrategy ParamStrategy::NonPointers() {
  auto f = [](const DomainConstructor::args &args) {
    params_t result;
    auto &fctx = *args.fctx;
    auto values = fctx.valuesAvailableIn(args.location, args.is_after_bb);

    for (auto value : values) {
      if (value->getType()->isPointerTy())
        continue;

      llvm::SmallVector<Expression, 2> p;
      p.push_back(value);
      result.push_back(p);
    }

    return result;
  };

  return ParamStrategy(1, f);
}

/**
 *  Checks for two llvm::Values whether they are used or defined in the same
 *  fragment.
 */
bool params::isInPack(symabs_ai::FragmentDecomposition &decomp, llvm::Value *a,
                      llvm::Value *b) {
  bool foundA = false, foundB = false;
  for (auto &frag : decomp) {
    for (auto *loc : frag.locations()) {
      if (loc == Fragment::EXIT)
        continue;
      for (auto &instr : *loc) {
        foundA = foundA || (&instr == a);
        foundB = foundB || (&instr == b);
        if (foundA && foundB)
          goto end;

        for (auto *other : instr.operand_values()) {
          foundA = foundA || (other == a);
          foundB = foundB || (other == b);
          if (foundA && foundB)
            goto end;
        }
      }
    }
    foundA = false;
    foundB = false;
  }
end:
  return foundA && foundB;
}

} // namespace symabs_ai
