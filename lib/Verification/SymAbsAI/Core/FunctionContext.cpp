/**
 * @file FunctionContext.cpp
 * @brief Builds the SMT encoding for a single function: represented values,
 *        edge predicates, and fragment-level path/semantic formulas.
 */
#include "Verification/SymAbsAI/Core/FunctionContext.h"

// #include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FloatingPointModel.h"
#include "Verification/SymAbsAI/Core/Fragment.h"
// #include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/MemoryModel.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Core/RepresentedValue.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
// #include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <queue>
#include <string>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/ValueSymbolTable.h>

namespace symabs_ai {
namespace // unnamed
{
struct ValueNameCompare {
  bool operator()(llvm::Value *a, llvm::Value *b) const {
    // give globals high ids to make ids a bit less configuration-dependent
    bool a_global = llvm::isa<llvm::GlobalVariable>(a);
    bool b_global = llvm::isa<llvm::GlobalVariable>(b);
    if (a_global && !b_global)
      return false;
    else if (!a_global && b_global)
      return true;
    else
      return a->getName().str() < b->getName().str();
  }
};
} // namespace

// prefix used for edge indicator variables to make sure they don't clash
// with state variables
const std::string EDGE_VAR_PREFIX = "__FROM_";

FunctionContext::FunctionContext(llvm::Function *func,
                                 const ModuleContext *mctx)
    : ModuleContext_(mctx), Function_(func),
      UndefinedBehaviorFlag_(getZ3().bool_const("__UNDEF_BEHAVIOR__")),
      Config_(ModuleContext_->getConfig()), DominatorTreePass_(nullptr) {
  using namespace llvm;
  MemoryModel_ = MemoryModel::New(*this);
  FloatingPointModel_ = FloatingPointModel::New(*this);

  // Use a set to avoid duplicates. Comparator needs to be reproducible since
  // the order determines RepresentedValue::id()
  std::set<Value *, ValueNameCompare> represented;

  // add all instructions and parameters to represented values (only those
  // that appear in the function's ValueSymbolTable, i.e. have a name).
  // Unnamed SSA values (e.g. many PHIs) are omitted unless
  // RepresentAllInstructions is true.
  for (auto &entry : *Function_->getValueSymbolTable()) {
    Value *val = entry.getValue();

    // Do not represent `invoke`. It's currently unsupported and
    // problematic since it's a terminator instruction but can also
    // define a value.
    if (isa<InvokeInst>(val))
      continue;

    Type *type = val->getType();
    if (type->isIntegerTy() || type->isPointerTy())
      represented.insert(val);

    if (type->isFloatingPointTy() && FloatingPointModel_->supportsType(type)) {
      represented.insert(val);
    }
  }

  if (Config_.get<bool>("FunctionContext", "RepresentAllInstructions", false)) {
    unsigned symabsId = 0;
    for (auto &BB : *Function_) {
      for (auto &I : BB) {
        if (represented.count(&I))
          continue;
        if (isa<InvokeInst>(&I))
          continue;
        Type *type = I.getType();
        if (type->isIntegerTy() || type->isPointerTy()) {
          if (I.getName().empty())
            I.setName("symabs." + std::to_string(symabsId++));
          represented.insert(&I);
        }
      }
    }
  }

  if (Config_.get<bool>("FunctionContext", "RepresentGlobals", true)) {
    // find all globals used in this function
    std::vector<Value *> used_globals;
    for (auto &global : func->getParent()->globals()) {
      bool used_here = false;
      for (auto *user : global.users()) {
        auto *as_instr = dyn_cast<llvm::Instruction>(user);
        if (as_instr && as_instr->getParent()->getParent() == func) {
          used_here = true;
          break;
        }
      }

      if (used_here)
        used_globals.push_back(&global);
    }

    // Globals and locals have separate namespaces in LLVM. We alter names
    // of locals in case of a name clash.
    std::set<std::string> global_names;
    for (auto *global : used_globals)
      global_names.insert(global->getName().str());

    for (auto *local : represented) {
      while (global_names.find(local->getName().str()) != global_names.end()) {

        local->setName(local->getName().str() + "_");
      }
    }

    // we can now safely add used_globals to RepresentedValues
    std::copy(used_globals.begin(), used_globals.end(),
              std::inserter(represented, represented.begin()));
  }

  // populate RepresentedValues_ with instances with appropriate indices
  unsigned i = 0;
  for (auto *value : represented) {
    assert(value->getName().str().length() > 0);
    RepresentedValues_.push_back(RepresentedValue(i, value));
    ++i;
  }
}

FunctionContext::FunctionContext(FunctionContext &&other) = default;
FunctionContext &FunctionContext::operator=(FunctionContext &&other) = default;

void FunctionContext::setMemoryModel(unique_ptr<MemoryModel> mem_model) {
  MemoryModel_ = std::move(mem_model);
}

// Returns an expression that is true if the non-phi part of bb is on the
// executed path. It's a disjunction of variables corresponding to bb's
// outgoing edges.
z3::expr
FunctionContext::getBasicBlockBodyCondition(const Fragment &frag,
                                            llvm::BasicBlock *bb) const {
  z3::expr result = getZ3().bool_val(false);

  if (bb == frag.getEnd() && frag.includesEndBody())
    return getZ3().bool_val(true);

  for (Fragment::edge e : frag.edgesFrom(bb))
    result = result || getEdgeVariable(e.first, e.second);

  return result;
}

// Returns an expression that is true if the phi part of bb is on the executed
// path. It's a disjunction of variables corresponding to bb's incoming edges.
z3::expr
FunctionContext::getBasicBlockPhiCondition(const Fragment &frag,
                                           llvm::BasicBlock *bb) const {
  z3::expr result = getZ3().bool_val(false);

  for (Fragment::edge e : frag.edgesTo(bb))
    result = result || getEdgeVariable(e.first, e.second);

  return result;
}

/**
 * Builds the core semantic formula for a fragment.
 *
 * The result is a conjunction of:
 *  - `code_formula`: per-instruction semantics guarded by path conditions
 *    for body and PHI parts of basic blocks,
 *  - `preservation_formula`: equalities preserving values not defined in
 *    the fragment between its entry and exit,
 *  - `mem_transfer`: memory-copy constraints along taken edges according
 *    to the chosen `MemoryModel`,
 *  - `cfg_formula`: structural constraints on edge variables encoding
 *    reachability and single-successor choices, and
 *  - `undef_formula` and `init_mem`: optional assumptions about undefined
 *    behavior and initialization of the memory model.
 */
z3::expr FunctionContext::formulaFor(const Fragment &frag) const {
  using namespace llvm;
  InstructionSemantics inst_sema(*this, frag);
  z3::expr code_formula = getZ3().bool_val(true);
  z3::expr preservation_formula = getZ3().bool_val(true);
  z3::expr mem_transfer = getZ3().bool_val(true);
  z3::expr cfg_formula = getZ3().bool_val(true);

  for (Fragment::edge edge : frag.edges()) {
    // construct the formula for the non-phi instructions
    {
      z3::expr executed = getZ3().bool_val(true);
      z3::expr not_executed = getZ3().bool_val(true);

      for (Instruction &inst : frag.edgeNonPhis(edge)) {
        if (isRepresentedValue(&inst)) {
          executed = executed && inst_sema.visit(inst);
          not_executed = not_executed && inst_sema.preserve(inst);
        } else {
          // necessary because of terminator instructions
          executed = executed && inst_sema.visit(inst);
        }
      }

      z3::expr condition = getBasicBlockBodyCondition(frag, edge.first);
      code_formula = code_formula && implies(condition, executed) &&
                     implies(!condition, not_executed);
    }

    // construct the formula for the phis
    {
      z3::expr executed = getZ3().bool_val(true);
      z3::expr not_executed = getZ3().bool_val(true);

      for (Instruction &inst : frag.edgePhis(edge)) {
        if (isRepresentedValue(&inst)) {
          executed = executed && inst_sema.visit(inst);
          not_executed = not_executed && inst_sema.preserve(inst);
        }
      }

      z3::expr condition = getBasicBlockPhiCondition(frag, edge.second);
      code_formula = code_formula && implies(condition, executed) &&
                     implies(!condition, not_executed);
    }
  }

  // construct the formula for instructions at the end of the terminating BB
  if (frag.includesEndBody()) {
    for (Instruction &inst : *frag.getEnd()) {
      if (!isa<PHINode>(inst))
        code_formula = code_formula && inst_sema.visit(inst);
    }
  }

  // create formulas for memory content transfer between locations
  for (llvm::BasicBlock *loc : frag.locations()) {
    for (auto edge : frag.edgesFrom(loc)) {
      auto vm_pre = ValueMapping::before(*this, frag, loc->getTerminator());

      auto vm_post = ValueMapping::atLocation(*this, frag, edge.second);
      auto copy_f = MemoryModel_->copy(vm_pre.memory(), vm_post.memory());
      mem_transfer = mem_transfer &&
                     implies(getEdgeVariable(edge.first, edge.second), copy_f);
    }
  }

  // preserve all function arguments and variables not defined in this
  // fragment
  auto vm_end = ValueMapping::atEnd(*this, frag);
  auto vm_start = ValueMapping::atBeginning(*this, frag);
  for (llvm::Value *value : representedValues()) {
    if (!frag.defines(value)) {
      preservation_formula =
          preservation_formula && vm_start.getFullRepresentation(value) ==
                                      vm_end.getFullRepresentation(value);
    }
  }

  cfg_formula = getBasicBlockBodyCondition(frag, frag.getStart());

  // initialize memory model SMT represenetation
  z3::expr init_mem = MemoryModel_->init_memory(vm_start.memory());

  // ensure continuity
  for (BasicBlock *bb : frag.locations()) {
    if (bb != frag.getEnd()) {
      cfg_formula =
          cfg_formula && implies(getBasicBlockPhiCondition(frag, bb),
                                 getBasicBlockBodyCondition(frag, bb));
    }

    if (bb != frag.getStart()) {
      cfg_formula = cfg_formula && implies(getBasicBlockBodyCondition(frag, bb),
                                           getBasicBlockPhiCondition(frag, bb));
    }
  }

  // make sure that at most one edge can be taken
  for (BasicBlock *bb_prev : frag.locations()) {
    if (bb_prev == Fragment::EXIT)
      continue;

    for (auto itr = succ_begin(bb_prev); itr != succ_end(bb_prev); ++itr) {
      BasicBlock *bb1 = *itr;
      z3::expr e = getZ3().bool_val(true);

      if (frag.edges().find({bb_prev, bb1}) == frag.edges().end())
        continue;

      for (auto itr = succ_begin(bb_prev); itr != succ_end(bb_prev); ++itr) {

        BasicBlock *bb2 = *itr;

        if (frag.edges().find({bb_prev, bb2}) == frag.edges().end())
          continue;

        if (bb1 != bb2)
          e = e && !getEdgeVariable(bb_prev, bb2);
      }

      cfg_formula = cfg_formula && implies(getEdgeVariable(bb_prev, bb1), e);
    }
  }

  z3::expr undef_formula = getZ3().bool_val(true);
  if (Config_.get<bool>("FunctionContext", "AssumeNoUndef", false)) {
    undef_formula = !UndefinedBehaviorFlag_;
  }

#ifndef NDEBUG
  vout << "FunctionContext::formulaFor code_formula {{{" << '\n'
       << code_formula << '\n'
       << "}}}" << '\n';
  if (is_unsat(code_formula))
    vout << "code_formula is UNSATISFIABLE" << '\n';

  vout << "FunctionContext::formulaFor preservation_formula {{{" << '\n'
       << preservation_formula << '\n'
       << "}}}" << '\n';
  if (is_unsat(preservation_formula))
    vout << "preservation_formula is UNSATISFIABLE" << '\n';

  vout << "FunctionContext::formulaFor mem_transfer {{{" << '\n'
       << mem_transfer << '\n'
       << "}}}" << '\n';

  vout << "FunctionContext::formulaFor cfg_formula {{{" << '\n'
       << cfg_formula << '\n'
       << "}}}" << '\n';
  if (is_unsat(cfg_formula))
    vout << "cfg_formula is UNSATISFIABLE" << '\n';

  vout << "FunctionContext::formulaFor undef_formula {{{" << '\n'
       << undef_formula << '\n'
       << "}}}" << '\n';
#endif

  return code_formula && preservation_formula && mem_transfer && cfg_formula &&
         undef_formula && init_mem;
}

RepresentedValue *
FunctionContext::findRepresentedValue(const llvm::Value *value) const {
  for (auto &rv : RepresentedValues_) {
    if (rv == value) {
      // const_cast is safe since RepresentedValue is immutable
      return const_cast<RepresentedValue *>(&rv);
    }
  }
  return nullptr;
}

z3::expr FunctionContext::getEdgeVariable(llvm::BasicBlock *bb_from,
                                          llvm::BasicBlock *bb_to) const {
  assert(bb_from != nullptr);
  // verify that this is a real edge
  bool is_real_edge = false;
  if (bb_to == Fragment::EXIT) {
    // from function end node to artificial nullptr-EXIT node
    is_real_edge = (succ_begin(bb_from) == succ_end(bb_from));
  } else {
    for (auto itr_bb_to = llvm::succ_begin(bb_from),
              end = llvm::succ_end(bb_from);
         itr_bb_to != end; ++itr_bb_to) {
      is_real_edge = is_real_edge || ((*itr_bb_to) == (bb_to));
    }
  }
  assert(is_real_edge);

  std::string name;

  if (bb_to == Fragment::EXIT) {
    name = EDGE_VAR_PREFIX + bb_from->getName().str() + "_TO__EXIT_";
  } else {
    name = EDGE_VAR_PREFIX + bb_from->getName().str() + "_TO_" +
           bb_to->getName().str();
  }

  return getZ3().bool_const(name.c_str());
}

z3::expr FunctionContext::getUndefinedBehaviorFlag() const {
  return UndefinedBehaviorFlag_;
}

int FunctionContext::getPointerSize() const {
  return ModuleContext_->getDataLayout()->getPointerSizeInBits(0);
}

z3::sort FunctionContext::sortForType(llvm::Type *type) const {
  if (type->isFloatingPointTy()) {
    return FloatingPointModel_->sortForType(type);
  }

  if (type->isPointerTy()) {
    return MemoryModel_->ptr_sort();
  }

  assert(type->isIntegerTy());
  unsigned bw = type->getIntegerBitWidth();
  return getZ3().bv_sort(bw);
}

unsigned int FunctionContext::bitsForType(llvm::Type *type) const {
  if (type->isPointerTy())
    return this->getPointerSize();

  if (type->isFloatingPointTy()) {
    llvm_unreachable("Bitwidth for floats not supported.");
  }

  assert(type->isIntegerTy());
  return type->getIntegerBitWidth();
}

z3::context &FunctionContext::getZ3() const { return ModuleContext_->getZ3(); }

const configparser::Config FunctionContext::getConfig() const {
  return Config_;
}

static bool dominates(llvm::DominatorTree *dt, llvm::BasicBlock *a,
                      llvm::BasicBlock *b) {
  if (b != nullptr)
    return dt->dominates(a, b);

  // for b == nullptr, check if a dominates every block without successors
  for (auto &bb : *a->getParent()) {
    if (llvm::succ_begin(&bb) == llvm::succ_end(&bb)) {
      if (!dt->dominates(a, &bb))
        return false;
    }
  }
  return true;
}

const std::vector<RepresentedValue> FunctionContext::parameters() const {
  std::vector<RepresentedValue> result;

  for (auto &arg : Function_->args()) {
    if (auto *rvptr = findRepresentedValue(&arg))
      result.push_back(*rvptr);
  }

  for (auto rv : RepresentedValues_) {
    llvm::Value *as_value = rv;
    auto *as_global = llvm::dyn_cast<llvm::GlobalVariable>(as_value);
    if (as_global != nullptr)
      result.push_back(rv);
  }

  return result;
}

llvm::DominatorTree &FunctionContext::getDomTree() const {
  if (DominatorTreePass_.get()) {
    return DominatorTreePass_->getDomTree();
  }
  DominatorTreePass_.reset(new llvm::DominatorTreeWrapperPass());
  DominatorTreePass_->runOnFunction(*Function_);
  return DominatorTreePass_->getDomTree();
}

std::vector<RepresentedValue>
FunctionContext::valuesAvailableIn(llvm::BasicBlock *bb, bool after) const {
  std::vector<RepresentedValue> result;
  llvm::DominatorTree &dt = this->getDomTree();

  for (RepresentedValue x : representedValues()) {
    llvm::Value *x_val = x;
    auto *x_inst = llvm::dyn_cast<llvm::Instruction>(x_val);

    // don't add uncovered instructions that are in a bb that is partially
    // covered
    if (x_inst && !after && (x_inst->getParent() == bb) &&
        !llvm::isa<llvm::PHINode>(x_inst)) {
      continue;
    }

    if (!x_inst || dominates(&dt, x_inst->getParent(), bb))
      result.push_back(x);
  }

  // sort values for reproducible results
  std::sort(result.begin(), result.end());

  return result;
}

// non-default destructor needed for unique_ptr on incomplete type
FunctionContext::~FunctionContext() {}
} // namespace symabs_ai
