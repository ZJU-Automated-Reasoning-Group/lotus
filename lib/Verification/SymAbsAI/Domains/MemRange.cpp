#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Expression.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ParamStrategy.h"
#include "Verification/SymAbsAI/Core/repr.h"
#include "Verification/SymAbsAI/Domains/Combinators.h"
#include "Verification/SymAbsAI/Domains/Intervals.h"
#include "Verification/SymAbsAI/Domains/NumRels.h"
#include "Verification/SymAbsAI/Domains/Product.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Module.h>
#include <z3++.h>

namespace symabs_ai {
namespace domains {
class PrintAsDereference {
public:
  virtual ~PrintAsDereference() = default;
  virtual void printAsDereference(PrettyPrinter &out) const = 0;
  virtual int accuracy() const { return 0; }
};

class AddrOffset : public AbstractValue, public PrintAsDereference {
private:
  enum state_t { TOP, BOTTOM, EQ_CONST, LE_CONST };

  const FunctionContext &FunctionContext_;
  state_t State_;
  ConcreteState::Value Constant_;
  RepresentedValue Base_;
  RepresentedValue Addr_;
  bool LastUpdateChanged_ = false;
  int WideningCooldown_ = 10;

  AddrOffset &operator=(const AddrOffset &other) = delete;
  AddrOffset(const AddrOffset &other) = default;

public:
  AddrOffset(const FunctionContext &fctx, RepresentedValue base,
             RepresentedValue addr)
      : FunctionContext_(fctx), State_(BOTTOM), Base_(base), Addr_(addr) {}

  virtual bool joinWith(const AbstractValue &av_other) override {
    auto &other = dynamic_cast<const AddrOffset &>(av_other);
    bool changed = false;

    if (isTop())
      return changed;

    if (other.isTop()) {
      changed = !isTop();
      havoc();
      return changed;
    }

    if (isBottom()) {
      State_ = other.State_;
      Constant_ = other.Constant_;
      changed = !other.isBottom();
      return changed;
    }

    if (other.isBottom())
      return changed;

    uint64_t this_c = Constant_;
    uint64_t other_c = other.Constant_;

    // take the bigger of two constant bounds
    if (other_c > this_c) {
      changed = true;
      Constant_ = other.Constant_;
    }

    // result is EQ_CONST only if both inputs are and constants are equal
    if (State_ != EQ_CONST || other.State_ != EQ_CONST || this_c != other_c) {

      if (!changed && State_ != LE_CONST)
        changed = true;

      State_ = LE_CONST;
    }

    return changed;
  }

  virtual bool meetWith(const AbstractValue &av_other) override {
    (void)av_other;
    assert(false && "not implemented");
    return false;
  }

  virtual bool updateWith(const ConcreteState &state) override {
    uint64_t addr = state[Addr_];
    uint64_t base = state[Base_];

    if (base > addr)
      return false;

    AddrOffset tmp(FunctionContext_, Base_, Addr_);
    unsigned bits = FunctionContext_.bitsForType(Addr_->getType());
    ConcreteState::Value constant(FunctionContext_, addr - base, bits);
    tmp.State_ = EQ_CONST;
    tmp.Constant_ = constant;

    // jump to top if the interval spans the entire address space
    if (base == 0 && addr + 1 == 0)
      tmp.havoc();

    LastUpdateChanged_ = joinWith(tmp);
    return LastUpdateChanged_;
  }

  virtual void widen() override {
    if (!LastUpdateChanged_) {
      WideningCooldown_--;
      if (WideningCooldown_ >= 0)
        return;
    }

    if (!isBottom())
      havoc();
  }

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override {
    using namespace z3;

    switch (State_) {
    case TOP:
      return zctx.bool_val(true);

    case BOTTOM:
      return ugt(vmap[Base_], vmap[Addr_]);

    case EQ_CONST:
      return implies(
          ule(vmap[Base_], vmap[Addr_]) &&
              ule(vmap[Base_], vmap[Base_] + static_cast<z3::expr>(Constant_)),
          vmap[Addr_] == vmap[Base_] + static_cast<z3::expr>(Constant_));

    case LE_CONST:
      return implies(
          ule(vmap[Base_], vmap[Addr_]) &&
              ule(vmap[Base_], vmap[Base_] + static_cast<z3::expr>(Constant_)),
          ule(vmap[Addr_], vmap[Base_] + static_cast<z3::expr>(Constant_)));
    }

    llvm_unreachable("invalid state");
  }

  virtual void prettyPrint(PrettyPrinter &out) const override {
    out << "AddrOffset(base=" << Base_ << ", addr=" << Addr_ << "): ";

    switch (State_) {
    case TOP:
      out << pp::top;
      return;

    case BOTTOM:
      out << pp::bottom;
      return;

    case EQ_CONST:
      out << Addr_ << " - " << Base_ << " = " << repr(Constant_);
      return;

    case LE_CONST:
      out << "(" << Addr_ << " - " << Base_ << ")" << pp::in << "[0, "
          << repr(Constant_) << "]";
      return;
    }
  }

  virtual void printAsDereference(PrettyPrinter &out) const override {
    if (State_ == TOP) {
      out << "<unknown>";
    } else if (State_ == BOTTOM) {
      out << "<never an address above " << Base_ << ">";
    } else {
      uint64_t constant = Constant_;

      if (State_ == EQ_CONST && constant == 0) {
        out << "*" << Base_;
      } else {
        if (State_ == EQ_CONST) {
          out << "*(" << Base_ << " + " << constant << ")";
        } else {
          out << "*[" << Base_ << ", " << Base_ << " + " << constant << "]";
        }
      }
    }
  }

  virtual int accuracy() const override {
    if (State_ == EQ_CONST)
      return 130;
    else if (State_ == TOP || State_ == BOTTOM)
      return 0;
    else
      return 70;
  }

  virtual AbstractValue *clone() const override {
    return new AddrOffset(*this);
  }

  virtual void havoc() override { State_ = TOP; }
  virtual bool isTop() const override { return State_ == TOP; }
  virtual bool isBottom() const override { return State_ == BOTTOM; }
  virtual void resetToBottom() override { State_ = BOTTOM; }

  virtual bool isJoinableWith(const AbstractValue &av_other) const override {
    auto *other = dynamic_cast<const AddrOffset *>(&av_other);
    if (other == nullptr)
      return false;
    else
      return Base_ == other->Base_ && Addr_ == other->Addr_;
  }
};

class RestrictedRelational : public Cut<RestrictedRelational, NumRels> {
public:
  RestrictedRelational(const FunctionContext &fctx, const Expression &left,
                       const Expression &right)
      : Cut<RestrictedRelational, NumRels>(
            make_unique<NumRels>(fctx, left, right)) {}

public:
  bool isInAllowedState() {
    return value().isTop() || value().isBottom() ||
           value().rel() == NumRels::EQUAL ||
           (value().rel() & NumRels::GREATER) == 0;
  }
};

class AddrVarOffset : public If, public PrintAsDereference {
private:
  const FunctionContext &FunctionContext_;
  RepresentedValue Base_, Addr_;
  Expression Candidate_;
  NumRels *RelBase_;
  NumRels *RelCandidate_;

  unique_ptr<AbstractValue> init(const FunctionContext &fctx,
                                 RepresentedValue base, RepresentedValue addr,
                                 const Expression &candidate, unsigned bytes) {
    assert(bytes != 0);
    auto product = make_unique<Product>(fctx);

    auto cv_bytes =
        ConcreteState::Value(fctx, bytes - 1, fctx.getPointerSize());

    // the maximal address of a (byte-sized) memory cell that might be
    // accessed
    auto max_addr = Expression(addr) + cv_bytes;

    auto *r_candidate =
        new RestrictedRelational(fctx, max_addr, Expression(base) + candidate);

    auto *r_base = new RestrictedRelational(fctx, base, addr);

    product->add(r_candidate);
    RelCandidate_ = &r_candidate->value();
    product->add(r_base);
    RelBase_ = &r_base->value();

    product->finalize();
    return std::move(product);
  }

public:
  AddrVarOffset(const FunctionContext &fctx, RepresentedValue base,
                RepresentedValue addr, const Expression &candidate,
                unsigned bytes)
      : If(Expression(base).ule(Expression(base) + Expression(candidate)),
           init(fctx, base, addr, candidate, bytes)),
        FunctionContext_(fctx), Base_(base), Addr_(addr),
        Candidate_(candidate) {}

  virtual void printAsDereference(PrettyPrinter &out) const override {
    if (isBottom()) {
      out << "<never>";
      return;
    }

    uint8_t rel_base = RelBase_->rel();

    if (rel_base == NumRels::EQUAL) {
      out << "*" << Base_;
      return;
    }

    out << "*[";
    if (rel_base == (NumRels::LOWER | NumRels::EQUAL))
      out << Base_;
    else if (rel_base == NumRels::LOWER)
      out << Base_ << "+1";
    else
      out << "?";

    out << ", ";

    uint8_t rel_cand = RelCandidate_->rel();
    if (rel_cand == (NumRels::LOWER | NumRels::EQUAL))
      out << Base_ << " + " << Candidate_;
    else if (rel_cand == NumRels::LOWER)
      out << Base_ << " + " << Candidate_ << "-1";
    else
      out << "?";

    out << "]";
  }

  virtual int accuracy() const override {
    int score = 0;

    if (!RelBase_->isTop())
      score += 50;

    if (!RelCandidate_->isTop())
      score += 50;

    return score;
  }
};

class MemoryAccessDescription : public Product {
private:
  llvm::Instruction *MemInstr_;
  RepresentedValue Addr_;
  bool PrintSourceNames_ = false;
  const FunctionContext *FunctionContext_;

  static llvm::Value *getMemInstAddress(llvm::Instruction *instr) {
    if (llvm::isa<llvm::LoadInst>(*instr))
      return instr->getOperand(0);
    else if (llvm::isa<llvm::StoreInst>(*instr))
      return instr->getOperand(1);
    else
      return nullptr;
  }

  unsigned getMemInstBytes(llvm::Instruction *instr) {
    llvm::Type *type;

    if (llvm::isa<llvm::LoadInst>(*instr)) {
      type = instr->getType();
    } else if (llvm::isa<llvm::StoreInst>(*instr)) {
      type = instr->getOperand(0)->getType();
    } else {
      return 0;
    }

    auto &ctx = FunctionContext_->getModuleContext();
    return ctx.getDataLayout()->getTypeStoreSize(type);
  }

public:
  MemoryAccessDescription(const FunctionContext &fctx, llvm::Instruction *instr,
                          const std::vector<RepresentedValue> &params)
      : Product(fctx), FunctionContext_(&fctx) {
    MemInstr_ = instr;
    Addr_ = *fctx.findRepresentedValue(getMemInstAddress(instr));
    unsigned bytes = getMemInstBytes(instr);

    for (auto base : params) {
      unsigned int base_bits = fctx.bitsForType(base->getType());
      unsigned int addr_bits = fctx.bitsForType(Addr_->getType());

      // TODO: better base heuristics
      if (!base->getType()->isPointerTy())
        continue;

      if (base_bits != addr_bits)
        continue;

      if (base_bits == addr_bits) {
        add(unique_ptr<AbstractValue>(new AddrOffset(fctx, base, Addr_)));
      }

      for (auto bound : params) {
        if (!bound->getType()->isIntegerTy())
          continue;

        unsigned int bound_bits = fctx.bitsForType(bound->getType());

        if (base == bound)
          continue;

        if (bound_bits < addr_bits) {
          auto ext = Expression(bound).signExtend(addr_bits);
          add(make_unique<AddrVarOffset>(fctx, base, Addr_, ext, bytes));
        } else {
          add(make_unique<AddrVarOffset>(fctx, base, Addr_, bound, bytes));
        }
      }
    }

    finalize();
  }

  virtual void gatherFlattenedSubcomponents(
      std::vector<const AbstractValue *> *result) const override {
    result->push_back(this);
  }

  virtual void prettyPrint(PrettyPrinter &out) const override {
    using namespace llvm;

    // Debug info disabled - just print address
    out << "[addr:" << Addr_ << "]";

    if (llvm::isa<llvm::StoreInst>(MemInstr_))
      out << " store ";
    else
      out << " load ";

    if (isBottom()) {
      out << "<never>"; // TODO: print under which assumptions
      return;
    }

    if (isTop()) {
      out << "<unknown>";
      return;
    }

    /* (useful for debugging accuracy ratings)
    out << "{\n";
    Product::prettyPrint(out);
    out << "}\n";
    return;
    */

    // rank values by accuracy
    std::vector<PrintAsDereference *> values;
    for (auto &avalue : getValues())
      values.push_back(dynamic_cast<PrintAsDereference *>(avalue.get()));

    std::sort(values.begin(), values.end(),
              [](PrintAsDereference *a, PrintAsDereference *b) {
                return a->accuracy() > b->accuracy();
              });

    // print only the best one
    values[0]->printAsDereference(out);
  }

  static std::vector<std::pair<RepresentedValue, RepresentedValue>>
  baseLengthPairs(const FunctionContext &fctx) {
    std::vector<std::pair<RepresentedValue, RepresentedValue>> result;

    // "end" or "last" style pointers like in walking_pointers.c
    for (auto &bb : *fctx.getFunction()) {
      for (auto &inst : bb) {
        auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst);

        if (gep != nullptr && gep->getNumOperands() == 2) {
          auto *base = gep->getOperand(0);
          auto *offset = gep->getOperand(1);
          RepresentedValue base_rv, offset_rv;

          for (auto &p : fctx.parameters()) {
            if (p == base)
              base_rv = p;

            if (p == offset)
              offset_rv = p;
          }

          if (base_rv != nullptr && offset_rv != nullptr)
            result.push_back({base_rv, offset_rv});
        }
      }
    }

    return result;
  }

  static unique_ptr<AbstractValue> CreateEverywhere(const FunctionContext &fctx,
                                                    llvm::BasicBlock *location,
                                                    bool after) {
    using namespace llvm;
    auto *function = fctx.getFunction();
    std::vector<RepresentedValue> parameters;

    // use function arguments as parameters
    for (auto &arg : function->args())
      parameters.push_back(*fctx.findRepresentedValue(&arg));

    // use represented (i.e. appearing in this function) globals
    for (auto &glob : function->getParent()->globals()) {
      auto *glob_rv = fctx.findRepresentedValue(&glob);
      if (glob_rv != nullptr)
        parameters.push_back(*fctx.findRepresentedValue(&glob));
    }

    // use results of loads as parameters
    for (auto value : fctx.valuesAvailableIn(location, after)) {
      if (isa<LoadInst>((Value *){value}))
        parameters.push_back(value);
    }

    // init the product
    auto product = make_unique<Product>(fctx);
    if (location != nullptr) {
      for (auto &instr : *location) {
        auto *addr_val = getMemInstAddress(&instr);

        if (addr_val == nullptr ||
            fctx.findRepresentedValue(addr_val) == nullptr) {

          continue;
        }

        if (isa<LoadInst>(instr) || isa<StoreInst>(instr)) {
          auto *mad = new MemoryAccessDescription(fctx, &instr, parameters);
          product->add(unique_ptr<AbstractValue>(mad));
        }
      }
    }
    product->finalize();

    return std::move(product);
  }

  static unique_ptr<AbstractValue>
  Create(const FunctionContext &fctx, llvm::BasicBlock *location, bool after) {
    unique_ptr<AbstractValue> result;

    if (after)
      return CreateEverywhere(fctx, location, after);

    // accompanying domains in abstraction points
    auto prod = make_unique<Product>(fctx);
    prod->add(params::ForValuePairs<NumRels>(fctx, location, after, true));

    // TODO: use baseLengthPairs()
    auto parameters = fctx.parameters();
    for (auto rv_a : parameters) {
      for (auto rv_b : parameters) {
        llvm::Type *ty_a = rv_a->getType();
        llvm::Type *ty_b = rv_b->getType();

        if (ty_a->isPointerTy() && ty_b->isIntegerTy()) {
          unsigned bits_a = fctx.bitsForType(ty_a);
          unsigned bits_b = fctx.bitsForType(ty_b);

          if (bits_b < bits_a)
            continue;

          auto bound = Expression(rv_a) + Expression(rv_b).zeroExtend(bits_a);

          for (auto left : fctx.valuesAvailableIn(location, after)) {
            if (!left->getType()->isPointerTy())
              continue;

            // TODO: ignore small values?
            prod->add(make_unique<NumRels>(fctx, left, bound));
          }
        }
      }
    }
    prod->finalize();
    result = std::move(prod);

    for (auto pair : baseLengthPairs(fctx)) {
      Expression base = pair.first;
      Expression len = pair.second;
      unsigned ptr_bits = fctx.getPointerSize();
      result = make_unique<If>(base.ule(base + len.zeroExtend(ptr_bits)),
                               std::move(result));
    }

    return result;
  }
};

namespace {
DomainConstructor::Register _("MemRange",
                              "describe memory accesses in terms of "
                              "function arguments and results of "
                              "other accesses",
                              &MemoryAccessDescription::Create);

DomainConstructor::Register
    _e("MemRange/Everywhere",
       "don't use the accompanying domains in abstraction points",
       &MemoryAccessDescription::CreateEverywhere);
} // namespace
} // namespace domains
} // namespace symabs_ai
