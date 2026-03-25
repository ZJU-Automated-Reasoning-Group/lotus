/**
 * @file SymAbsAIPass.cpp
 * @brief Implementation of the SymAbsAI optimization pass on top of
 * SymAbsAI abstract interpretation results.
 */

#include "Verification/SymAbsAI/Core/SymAbsAIPass.h"

#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Domains/Boolean.h"
#include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Domains/SimpleConstProp.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <deque>
#include <iostream>

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/ValueSymbolTable.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

using namespace llvm;

#define DEBUG_TYPE "symabs-ai-pass"
STATISTIC(NumReplacedUses, "Number of replaced uses");

namespace symabs_ai {
namespace domains {
class EqDomain : public BooleanValue {
private:
  const RepresentedValue Left_;
  const RepresentedValue Right_;

protected:
  virtual z3::expr makePredicate(const ValueMapping &vmap) const override {
    return vmap[Left_] == vmap[Right_];
  }

public:
  EqDomain(const FunctionContext &fctx, const RepresentedValue left,
           const RepresentedValue right)
      : BooleanValue(fctx), Left_(left), Right_(right) {}

  llvm::Value *getLeftVal() const { return Left_; }
  llvm::Value *getRightVal() const { return Right_; }

  virtual void prettyPrint(PrettyPrinter &out) const override {
    std::string left_name = Left_->getName().str();
    std::string right_name = Right_->getName().str();

    switch (Val_) {
    case BOTTOM:
      out << pp::bottom;
      break;
    case TOP:
      out << pp::top;
      break;
    case TRUE:
      out << left_name << " == " << right_name;
      break;
    case FALSE:
      out << left_name << " != " << right_name;
      break;
    }
  }

  virtual ~EqDomain() {}

  virtual AbstractValue *clone() const override { return new EqDomain(*this); }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    auto *other_val = dynamic_cast<const EqDomain *>(&other);
    if (other_val) {
      if (other_val->Left_ == Left_ && other_val->Right_ == Right_) {
        return true;
      }
    }
    return false;
  }
};
} // namespace domains
namespace // unnamed
{
using namespace llvm;

/**
 *  Check whether the given AbstractValue contains an AbstractValue belonging
 *  to the template parameter.
 *
 *  Example call:
 *      containsDomain<SimpleConstProp>(val_ptr)
 */
template <typename T>
bool containsDomain(const symabs_ai::AbstractValue *value) {
  bool res = false;

  // get flat vector of resulting AbstractValues
  std::vector<const AbstractValue *> vec;
  value->gatherFlattenedSubcomponents(&vec);
  for (const auto *inner_val : vec) {
    if (dynamic_cast<const T *>(inner_val)) {
      res = true;
      break;
    }
  }
  return res;
}
} // namespace

char SymAbsAIPass::ID;

SymAbsAIPass::SymAbsAIPass() : llvm::FunctionPass(SymAbsAIPass::ID) {
  const char *M = "SymAbsAIPass";

  Config_.ConstantPropagation =
      GlobalConfig_.get<bool>(M, "ConstantPropagation", true);
  Config_.RedundantComputationRemoval =
      GlobalConfig_.get<bool>(M, "RedundantComputationRemoval", false);
  Config_.Verbose = GlobalConfig_.get<bool>(M, "Verbose", false);

  VerboseEnable = Config_.Verbose;
}

/**
 * Ensures that the abstract domain used by the analyzer contains the
 * components needed by this pass.
 *
 * Starting from the domain specified in the configuration, this function
 * checks whether it already includes constant-propagation and equality
 * predicate components. If not, it wraps the base domain into a product
 * with the missing domains so that analysis results expose the information
 * required for the transformations implemented below.
 */
DomainConstructor
SymAbsAIPass::getAugmentedDomain(symabs_ai::FunctionContext &smtsem) {
  using namespace symabs_ai;
  using namespace domains;

  DomainConstructor domain(smtsem.getConfig());

  bool contains_cp = containsDomain<SimpleConstProp>(
      domain.makeBottom(smtsem, Fragment::EXIT, false).get());

  bool contains_eqres = containsDomain<EqDomain>(
      domain.makeBottom(smtsem, Fragment::EXIT, false).get());

  bool needs_cp = Config_.ConstantPropagation && !contains_cp;
  bool needs_eqres = Config_.RedundantComputationRemoval && !contains_eqres;

  if (needs_cp && needs_eqres) {
    vout << "Adding SimpleConstProp and EqRes to domain." << '\n';
    return DomainConstructor(
        domain.name() + "+consts", "",
        [=](const FunctionContext &fctx, BasicBlock *for_bb, bool after) {
          domains::Product *p = new domains::Product(fctx);
          p->add(domain.makeBottom(fctx, for_bb, after));
          p->add(params::ForValues<SimpleConstProp>(fctx, for_bb, after));
          p->add(
              params::ForValuePairsRestricted<EqDomain>(fctx, for_bb, after));
          p->finalize();
          return unique_ptr<AbstractValue>(p);
        });
  }
  if (needs_cp) {
    vout << "Adding SimpleConstProp to domain." << '\n';
    return DomainConstructor(
        domain.name() + "+consts", "",
        [=](const FunctionContext &fctx, BasicBlock *for_bb, bool after) {
          domains::Product *p = new domains::Product(fctx);
          p->add(domain.makeBottom(fctx, for_bb, after));
          p->add(params::ForValues<SimpleConstProp>(fctx, for_bb, after));
          p->finalize();
          return unique_ptr<AbstractValue>(p);
        });
  }
  if (needs_eqres) {
    vout << "Adding EqRes to domain." << '\n';
    return DomainConstructor(
        domain.name() + "+consts", "",
        [=](const FunctionContext &fctx, BasicBlock *for_bb, bool after) {
          domains::Product *p = new domains::Product(fctx);
          p->add(domain.makeBottom(fctx, for_bb, after));
          p->add(
              params::ForValuePairsRestricted<EqDomain>(fctx, for_bb, after));
          p->finalize();
          return unique_ptr<AbstractValue>(p);
        });
  }

  return domain;
}

bool SymAbsAIPass::replaceUsesOfWithInBBAndPHISuccs(BasicBlock &bb, Value *from,
                                                    Value *to) {
  bool changed = false;

  // get a reasonable textual representation of a Value
  auto getText = [](Value *val) -> std::string {
    if (auto *v = dyn_cast_or_null<Constant>(val)) {
      if (llvm::isa<ConstantPointerNull>(v))
        return "nullptr";
      else if (llvm::isa<ConstantExpr>(v))
        return "some constant expression";
      else
        return std::to_string(v->getUniqueInteger().getZExtValue());
    } else {
      return "`" + val->getName().str() + "`";
    }
  };

  for (auto &inst : bb) {
    bool found_use = false;
    for (auto *arg : inst.operand_values()) {
      if (arg == from) {
        found_use = true;
        ++NumReplacedUses;
      }
    }
    if (found_use) {
      // we found a use. This is relevant for the value of changed
      inst.replaceUsesOfWith(from, to);
      changed = true;
      vout << "  Replaced use of `" << from->getName().str() << "` by value "
           << getText(to) << " in `" << inst.getName().str() << "` (bb: `"
           << bb.getName().str() << "`)" << '\n';
    }
  }

  // Replace constant arguments of PHIs in successor bbs.
  // This might be necessary as they might not be contained
  // in the successors AbstractValue.
  for (auto it = succ_begin(&bb); it != succ_end(&bb); ++it) {
    for (auto &inst : **it) {
      if (auto *phi = dyn_cast<PHINode>(&inst)) {
        if (phi->getIncomingValueForBlock(&bb) == from) {
          int idx = phi->getBasicBlockIndex(&bb);
          phi->setIncomingValue(idx, to);
          changed = true;
          ++NumReplacedUses;
          vout << "  Replaced PHI use of `" << from->getName().str()
               << "` by value " << getText(to) << " in `"
               << inst.getName().str() << "` (bb: `" << bb.getName().str()
               << "`)" << '\n';
        }
      } else {
        break;
      }
    }
  }
  return changed;
}

/**
 * Given a proven constant-propagation fact, replaces uses of the variable
 * with a concrete LLVM constant of matching type.
 *
 * The replacement is restricted to the current basic block and any PHI
 * incoming values in successors so that SSA form remains valid and all
 * transformed uses are control-flow reachable from the defining block.
 */
bool SymAbsAIPass::performConstPropForBB(
    const FunctionContext &fctx, BasicBlock &bb,
    const symabs_ai::domains::SimpleConstProp *scp) {
  if (!scp->isConst())
    return false;

  llvm::Value *var = scp->getVariable();
  uint64_t val = scp->getConstValue();
  z3::sort sort = fctx.sortForType(var->getType());

  if (!sort.is_bv())
    return false;

  int bw = sort.bv_size();

  // Create llvm constant with identical type to eliminate use
  auto *type = var->getType();
  auto apval = APInt(bw, val, false);
  Value *const_int = Constant::getIntegerValue(type, apval);

  return replaceUsesOfWithInBBAndPHISuccs(bb, var, const_int);
}

void SymAbsAIPass::insertEquality(equals_t &eqs, Value *a, Value *b) {
  // If x is presenet in a set of eqs, inserts y into the same set and
  // returns `true`. Returns `false` otherwise.
  auto insertConditionally = [&eqs](Value *x, Value *y) {
    for (auto &part : eqs) {
      if (part.find(x) != part.end()) {
        part.insert(y);
        return true;
      }
    }
    return false;
  };

  if (!insertConditionally(a, b) && !insertConditionally(b, a)) {
    // Create new class of equal Values
    eqs.emplace_back();
    eqs[eqs.size() - 1].insert(a);
    eqs[eqs.size() - 1].insert(b);
  }
}

Value *SymAbsAIPass::getReplacementCanditdate(const equals_t &eqs, Value *val) {
  Instruction *candidate = dyn_cast_or_null<Instruction>(val);
  if (!candidate)
    return nullptr; // we only want to replace instructions

  llvm::DominatorTreeWrapperPass pass;
  pass.runOnFunction(*candidate->getParent()->getParent());
  llvm::DominatorTree &dt = pass.getDomTree();

  for (auto &eq : eqs) {
    // find the set that contains val
    if (eq.find(val) != eq.end()) {
      // find most dominating Value in set
      for (auto *oth : eq) {
        if (auto *oth_inst = dyn_cast_or_null<Instruction>(oth)) {
          if (dt.dominates(oth_inst, candidate)) {
            candidate = oth_inst;
          }
        } else {
          return oth;
          // replacing with something that is not an instruction
          // is always good as it means no recomputation
        }
      }
      break;
    }
  }
  if (candidate == val)
    return nullptr;
  return candidate;
}

/**
 * Performs redundancy elimination in a single basic block given a partition
 * of equal values.
 *
 * For each equivalence class, this function chooses a dominating
 * representative (prefer non-instructions when available to avoid
 * recomputation) and rewrites uses of other members in the block and
 * successor PHIs to that representative. This can eliminate duplicated
 * computations without changing semantics.
 */
bool SymAbsAIPass::performRedundancyReplForBB(const equals_t &eqs,
                                              BasicBlock &bb) {
  using namespace symabs_ai;
  using namespace domains;
  std::map<Value *, Value *> repl;
  bool changed = false;

  // Try to compute for each value another value which we can replace it with
  vout << "  equalities for " << bb.getName().str() << ": [" << '\n';
  for (auto &eq : eqs) {
    vout << "    [";
    bool first = true;
    for (auto *val : eq) {
      if (!first)
        vout << ", ";
      first = false;
      vout << val->getName().str();
      repl[val] = getReplacementCanditdate(eqs, val);
      vout << " -> " << (repl[val] ? repl[val]->getName().str() : "NONE");
    }
    vout << "]" << '\n';
  }
  vout << "  ]" << '\n';

  // Perform the replacements with the values that we found
  for (auto &eq : eqs) {
    for (auto *val : eq) {
      if (repl[val]) {
        bool tmp = replaceUsesOfWithInBBAndPHISuccs(bb, val, repl[val]);
        changed = changed || tmp;
      }
    }
  }

  return changed;
}

/**
 * Main entry point of the SymAbsAI optimization pass.
 *
 * For the current function, this wires together:
 *  - creation of `ModuleContext` and `FunctionContext`,
 *  - construction of a fragment decomposition and augmented domain,
 *  - running the SymAbsAI analyzer, and
 *  - applying constant propagation and redundancy elimination
 *    based on the abstract results for each basic block.
 */
bool SymAbsAIPass::runOnFunction(llvm::Function &function) {
  vout << "Perform SymAbsAIPass on function `" << function.getName().str()
       << "'." << '\n'
       << "¸.·´¯`·.´¯`·.¸¸.·´¯`·.¸><(((º>" << '\n'
       << '\n';
  bool changed = false;

  using namespace symabs_ai;
  using namespace domains;

  // Create a ModuleContext object to create FunctionContexts
  auto mctx =
      make_unique<const ModuleContext>(function.getParent(), GlobalConfig_);

  // Create the FunctionContext object that is used for the analysis
  auto fctx = mctx->createFunctionContext(&function);

  // Generate the FragmentDecomposition that is specified by the Config_ field
  // of the FunctionContext
  auto fragment_decomp = FragmentDecomposition::For(*fctx.get());
  vout << "Fragment decomposition: " << fragment_decomp << '\n';

  // add necessary components to domain if not yet contained
  DomainConstructor domain = getAugmentedDomain(*fctx.get());
  auto analyzer = Analyzer::New(*fctx.get(), fragment_decomp, domain);

  std::vector<const AbstractValue *> results;
  equals_t equalities;

  vout << "Analysis Results {{{" << '\n';
  for (llvm::BasicBlock &bb : function) {
    results.clear();
    equalities.clear();

    // Compute and gather the analysis results for this basic block
    analyzer->after(&bb)->gatherFlattenedSubcomponents(&results);

    // Perform the actual transformations for Constant Replacement
    // and find equal values for Redundant Computation Elimination
    for (auto &val : results) {
      if (const auto *scp = dynamic_cast<const SimpleConstProp *>(val)) {
        // Constant Replacement transformation
        if (!Config_.ConstantPropagation)
          continue; // Do not perform this transformation
        bool res = performConstPropForBB(*fctx.get(), bb, scp);
        changed = changed || res;
      } else if (const auto *pred = dynamic_cast<const EqDomain *>(val)) {
        // Redundant Computation Elimination transformation
        if (!Config_.RedundantComputationRemoval)
          continue; // Do not perform this transformation
        if (pred->getValue() == BooleanValue::TRUE) {
          insertEquality(equalities, pred->getLeftVal(), pred->getRightVal());
        }
      }
    }

    if (Config_.RedundantComputationRemoval) {
      // Redundant Computation Elimination transformation
      // perform the actual transformation
      bool res = performRedundancyReplForBB(equalities, bb);
      changed = changed || res;
    }
  }

  vout << "}}}" << '\n' << "DONE." << '\n';
  return changed;
}
} // namespace symabs_ai
