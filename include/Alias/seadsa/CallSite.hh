#ifndef __DSA_CALLSITE_HH_
#define __DSA_CALLSITE_HH_

#include "llvm/ADT/Optional.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/Pass.h"

// XXX: included for Cell.
#include "Alias/seadsa/Graph.hh"

#include "boost/iterator/filter_iterator.hpp"

namespace llvm {
class Value;
class Function;
class Instruction;
} // namespace llvm

namespace seadsa {

class DsaCallSite {

  struct isPointerTy {
    bool operator()(const llvm::Value *v);
    bool operator()(const llvm::Argument &a);
  };

  // -- the call site
  const llvm::CallBase *m_cb;
  // -- the cell to which the indirect callee function points to.
  //    it has no value if the callsite is direct.
  llvm::Optional<Cell> m_cell;
  // -- whether the callsite has been cloned: used only internally during
  // -- bottom-up pass.
  bool m_cloned;
  // -- class invariant: m_callee != nullptr
  const llvm::Function *m_callee;

public:
  using const_formal_iterator =
      boost::filter_iterator<isPointerTy,
                             typename llvm::Function::const_arg_iterator>;
  using const_actual_iterator =
      boost::filter_iterator<isPointerTy,
                             typename llvm::User::const_op_iterator>;

  DsaCallSite(const llvm::CallBase &cb);
  DsaCallSite(const llvm::Instruction &cs);
  DsaCallSite(const llvm::Instruction &cs, Cell c);
  DsaCallSite(const llvm::Instruction &cs, const llvm::Function &callee);

  bool operator==(const DsaCallSite &o) const {
    return (getInstruction() == o.getInstruction() &&
            getCallee() == o.getCallee());
  }

  bool operator<(const DsaCallSite &o) const {
    if (getInstruction() < o.getInstruction()) {
      return true;
    } else if (getInstruction() == o.getInstruction()) {
      return getCallee() < o.getCallee();
    } else {
      return false;
    }
  }

  bool hasCell() const;
  const Cell &getCell() const;
  Cell &getCell();

  const llvm::Value &getCalledValue() const;

  bool isIndirectCall() const;
  bool isInlineAsm() const;
  bool isCloned() const;

  void markCloned(bool v = true);

  const llvm::Value *getRetVal() const;

  const llvm::Function *getCallee() const;
  const llvm::Function *getCaller() const;

  const llvm::Instruction *getInstruction() const;

  const llvm::CallBase &getCallBase() const { return *m_cb; }

  const_formal_iterator formal_begin() const;
  const_formal_iterator formal_end() const;

  const_formal_iterator formal_func_begin(const llvm::Function &F) const;
  const_formal_iterator formal_func_end(const llvm::Function &F) const;

  llvm::iterator_range<const_formal_iterator> formals() const {
    return llvm::make_range(formal_begin(), formal_end());
  }

  const_actual_iterator actual_begin() const;
  const_actual_iterator actual_end() const;
  llvm::iterator_range<const_actual_iterator> actuals() const {
    return llvm::make_range(actual_begin(), actual_end());
  }

  void write(llvm::raw_ostream &o) const;
};

} // namespace seadsa
#endif
