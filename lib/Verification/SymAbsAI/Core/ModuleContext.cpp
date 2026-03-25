/**
 * @file ModuleContext.cpp
 * @brief Module-level SymAbsAI integration: owns the Z3 context,
 * dynamic result database, and builds formulas/summaries for LLVM functions.
 */
#include "Verification/SymAbsAI/Core/ModuleContext.h"

#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/Fragment.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <llvm/ADT/Triple.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace symabs_ai {
using llvm::LibFunc;

std::string ModuleContext::readGlobalString(llvm::Module *module,
                                            const char *name) {
  using namespace llvm;

  auto *global = module->getGlobalVariable(name);
  if (!global)
    return "";

  auto *init = global->getInitializer();
  auto *carr = dyn_cast_or_null<ConstantDataArray>(init);
  if (carr != nullptr && carr->isCString())
    return carr->getAsCString().str();
  else
    return "";
}

ModuleContext::ModuleContext(llvm::Module *module, configparser::Config config)
    : Module_(module), Config_(config), Z3Context_(make_unique<z3::context>()) {
  auto database_path =
      readGlobalString(Module_, "symbolic_abstraction_rt_database_path");
  // incorporate dynamic analysis results if available
  if (database_path.size() > 0) {
    vout << "Using dynamic results from `" << database_path << "`" << '\n';
    Store_.reset(new ResultStore(database_path));
  }

  DataLayout_.reset(new llvm::DataLayout(Module_));

  // In LLVM 12/14, TargetLibraryInfo constructor changed
  llvm::TargetLibraryInfoImpl TLII(llvm::Triple(Module_->getTargetTriple()));
  TLI_.reset(new llvm::TargetLibraryInfo(TLII));
}

z3::expr ModuleContext::formulaForBuiltin(llvm::Function *function) const {
  // TODO: support some builtins?
  return Z3Context_->bool_val(true);
}

z3::expr ModuleContext::substituteReturn(z3::expr formula, ValueMapping vmap,
                                         llvm::ReturnInst *ret) const {
  if (ret->getNumOperands() == 0)
    return formula;

  auto *as_arg = llvm::dyn_cast<llvm::Argument>(ret->getOperand(0));
  auto *as_rv = vmap.fctx().findRepresentedValue(ret->getOperand(0));
  auto *as_const = llvm::dyn_cast<llvm::ConstantInt>(ret->getOperand(0));

  if (as_arg != nullptr || as_const != nullptr) {
    // The function returns either one of the formal arguments or a numeric
    // constant. In both cases we can produce an expression `ret_expr` that
    // doesn't refer to internal variables and can be directly inserted
    // into returned formula.

    z3::expr ret_expr(*Z3Context_);
    if (as_const != nullptr) {
      unsigned bits = as_const->getBitWidth();
      ret_expr = ConcreteState::Value(Z3Context_.get(),
                                      as_const->getZExtValue(), bits);
    } else {
      assert(as_rv != nullptr);
      ret_expr = vmap[*as_rv];
    }

    z3::expr ret_sym =
        Z3Context_->constant(getReturnSymbol(), ret_expr.get_sort());
    return formula && (ret_sym == ret_expr);
  }

  if (as_rv != nullptr) {
    // The function returns a represented value. Substitute its name for
    // __RETURN__.
    z3::expr_vector src(*Z3Context_), dst(*Z3Context_);
    src.push_back(vmap.getFullRepresentation(*as_rv));
    auto sort = vmap.fctx().sortForType((*as_rv)->getType());
    dst.push_back(Z3Context_->constant(getReturnSymbol(), sort));
    return formula.substitute(src, dst);
  }

  return formula;
}

unique_ptr<FunctionContext>
ModuleContext::createFunctionContext(llvm::Function *f) const {
  return make_unique<FunctionContext>(f, this);
}

std::set<z3::symbol>
ModuleContext::getSharedSymbols(FunctionContext *fctx) const {
  std::set<z3::symbol> result;
  auto &zctx = fctx->getZ3();

  // special symbol __RETURN__ used to bind the return value
  result.insert(getReturnSymbol());

  // all formal arguments of represented types
  for (auto &arg : fctx->getFunction()->args()) {
    if (fctx->findRepresentedValue(&arg) != nullptr) {
      std::string name_str = arg.getName().str();
      const char *name = name_str.c_str();
      z3::symbol s(zctx, Z3_mk_string_symbol(zctx, name));
      result.insert(s);
    }
  }

  // TODO: globals?
  return result;
}

z3::expr ModuleContext::formulaFor(llvm::Function *function) const {
  assert(function != nullptr);
  bool recursive = Config_.get<bool>("ModuleContext", "Recursive", false);

  // avoid infinite recursion if we're already generating a formula for this
  // function
  if (RecurFuncs_.find(function) != RecurFuncs_.end())
    return Z3Context_->bool_val(true);

  RecurFuncs_.insert(function);
  z3::expr result = Z3Context_->bool_val(true);

  if (function->begin() == function->end())
    result = formulaForBuiltin(function);
  else if (recursive) {
    VOutBlock vb("Recursively analyzing callee");
    vout << "Function name: " << function->getName().str() << '\n';

    FunctionContext fctx(function, this);
    auto fragment_decomp = FragmentDecomposition::For(fctx);
    DomainConstructor domain(fctx.getConfig());
    auto analyzer = Analyzer::New(fctx, fragment_decomp, domain);
    vout << "Fragment decomposition: " << fragment_decomp << '\n';
    auto symbs = getSharedSymbols(&fctx);

    // `result` will be a disjunction of formulas for different exit BBs
    // and fragments
    result = Z3Context_->bool_val(false);

    for (auto &bb : *function) {
      auto *ret = llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator());
      if (ret == nullptr)
        continue;

      std::vector<const AbstractValue *> sub_comp;
      analyzer->after(&bb)->gatherFlattenedSubcomponents(&sub_comp);

      for (auto &frag : fragment_decomp) {
        if (frag.locations().find(&bb) == frag.locations().end())
          continue;

        auto vmap = ValueMapping::before(fctx, frag, ret);

        // `conj` is a conjunction of formulas for different
        // subcomponents of `avalue`
        z3::expr conj = Z3Context_->bool_val(true);
        for (auto *avalue : sub_comp) {
          auto formula = avalue->toFormula(vmap, getZ3());
          formula = substituteReturn(formula, vmap, ret);

          // skip if the expression contains disallowed symbols
          bool is_allowed = true;
          for (auto &x : expr_constants(formula)) {
            if (symbs.find(x.decl().name()) == symbs.end()) {
              is_allowed = false;
              break;
            }
          }

          if (is_allowed)
            conj = conj && formula;
        }

        result = result || conj;
      }
    }

    vout << "Function summary: " << repr(result) << '\n';
  }

  RecurFuncs_.erase(function);
  return result;
}
} // namespace symabs_ai
