#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"

#include "z3++.h"

#include <iostream>
#include <map>
#include <unordered_map>

#ifndef LLMAPPING
#define LLMAPPING std::map<std::string, Value *>
#endif

using namespace llvm;
using namespace z3;

namespace SLOT {
using SMTValueCache = std::unordered_map<unsigned, Value *>;

class IntegerNode;
class FloatingNode;
class BitvectorNode;
class BooleanNode;
class RealNode;

class SMTNode {
public:
  LLVMContext &lcx;
  Module *lmodule;
  IRBuilder<> &builder;
  unsigned integer_width;
  const LLMAPPING &variables;
  SMTValueCache *value_cache;
  expr contents;
  bool noOverflow = true;
  mutable bool m_has_stripped_name = false;
  mutable std::string m_stripped_name;
  mutable int m_symbol_kind =
      -1; // -1 unknown, 0 non-const, 1 variable, 2 constant

  // Z3 ''constants'' can be either variables or constants
  inline const std::string &StrippedName() {
    if (!m_has_stripped_name) {
      std::string repr = contents.to_string();
      if (!repr.empty() && repr[0] == '|') {
        m_stripped_name = repr.substr(1, repr.length() - 2);
      } else {
        m_stripped_name = std::move(repr);
      }
      m_has_stripped_name = true;
    }
    return m_stripped_name;
  }
  inline int SymbolKind() {
    if (m_symbol_kind != -1) {
      return m_symbol_kind;
    }
    if (!contents.is_const()) {
      m_symbol_kind = 0;
      return m_symbol_kind;
    }
    m_symbol_kind = (variables.find(StrippedName()) != variables.end()) ? 1 : 2;
    return m_symbol_kind;
  }
  inline bool IsVariable() { return SymbolKind() == 1; }
  inline bool IsConstant() { return SymbolKind() == 2; }
  inline Z3_decl_kind Op() { return contents.decl().decl_kind(); }

  inline Z3_decl_kind RoundingMode() {
    return ((contents.arg(0).get_sort().sort_kind() == Z3_ROUNDING_MODE_SORT)
                ? contents.arg(0).decl().decl_kind()
                : Z3_OP_FPA_RM_NEAREST_TIES_TO_EVEN);
  }
  inline bool IsRNE() {
    return RoundingMode() == Z3_OP_FPA_RM_NEAREST_TIES_TO_EVEN;
  }

  MetadataAsValue *LLVMRoundingMode();
  MetadataAsValue *LLVMException();

  // Syntax sugar for extracting children
  inline FloatingNode FloatingChild(expr cont);
  inline FloatingNode FloatingChild(int index);
  inline BitvectorNode BitvectorChild(expr cont);
  inline BitvectorNode BitvectorChild(int index);
  inline BooleanNode BooleanChild(expr cont);
  inline BooleanNode BooleanChild(int index);

  SMTNode(LLVMContext &t_lcx, Module *t_lmodule, IRBuilder<> &t_builder,
          const LLMAPPING &t_variables, SMTValueCache *t_value_cache,
          expr t_contents);
  virtual ~SMTNode() {}
  Value *ToLLVM();
  virtual Value *ToLLVMInternal() = 0;
};

//--------------------------------------------------------------------------------

class FloatingNode : public SMTNode {
public:
  static const std::map<Z3_decl_kind, int> class_flags;

  inline unsigned SBits() { return contents.get_sort().fpa_sbits(); }
  inline unsigned Width() {
    return contents.get_sort().fpa_sbits() + contents.get_sort().fpa_ebits();
  }

  static Type *ToFloatingType(LLVMContext &lcx, std::string name,
                              unsigned width);
  inline Type *FloatingType() {
    return FloatingNode::ToFloatingType(lcx, contents.to_string(), Width());
  }

  Value *LLVMClassCheck(Z3_decl_kind op);
  Value *LLVMEq(FloatingNode other);
  Value *LLVMNE(FloatingNode other);

  Value *ToLLVMInternal() override;
  FloatingNode(LLVMContext &t_lcx, Module *t_lmodule, IRBuilder<> &t_builder,
               const LLMAPPING &t_variables, SMTValueCache *t_value_cache,
               expr t_contents);
};

//--------------------------------------------------------------------------------

class BitvectorNode : public SMTNode {
public:
  inline unsigned Width() { return contents.get_sort().bv_size(); }
  inline Value *Zero() {
    return ConstantInt::get(IntegerType::get(lcx, Width()), 0);
  }

  inline Value *IsZero() { return builder.CreateICmpEQ(ToLLVM(), Zero()); }
  inline Value *IsNegative() { return builder.CreateICmpSLT(ToLLVM(), Zero()); }
  inline Value *IsPositive() { return builder.CreateICmpSGE(ToLLVM(), Zero()); }

  static Value *LlURem(IRBuilder<> &builder, Value *left, Value *right);

  Value *ToLLVMInternal() override;
  BitvectorNode(LLVMContext &t_lcx, Module *t_lmodule, IRBuilder<> &t_builder,
                const LLMAPPING &t_variables, SMTValueCache *t_value_cache,
                expr t_contents);
};

//--------------------------------------------------------------------------------

class BooleanNode : public SMTNode {
public:
  Value *ToLLVMInternal() override;

  BooleanNode(LLVMContext &t_lcx, Module *t_lmodule, IRBuilder<> &t_builder,
              const LLMAPPING &t_variables, SMTValueCache *t_value_cache,
              expr t_contents);
};
} // namespace SLOT
