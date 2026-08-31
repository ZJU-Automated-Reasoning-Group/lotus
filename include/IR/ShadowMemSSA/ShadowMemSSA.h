#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"

#include <map>

/* Notes about sea_dsa::ShadowMem

   sea_dsa does not provide a direct API to access to memory SSA
   form. Instead, sea_dsa ShadowMem pass instruments the code with
   shadow pseudo functions from which we can extract def-use chains
   from memory:

   (1)  shadow.mem.load(NodeID, TLVar, SingletonGlobal)
   (2)  TLVar shadow.mem.store(NodeID, TLVar, SingletonGlobal)
   (3)  shadow.mem.arg.ref(NodeID, TLVar, Idx, SingletonGlobal)
   (4)  TLVar shadow.mem.arg.mod(NodeID, TLVar, Idx, SingletonGlobal)
   (5)  TLVar shadow.mem.arg.ref_mod(NodeID, TLVar, Idx, SingletonGlobal)
   (6)  TLVar shadow.mem.arg.new(NodeID, TLVar, Idx, SingletonGlobal)
   (7)  shadow.mem.in(NodeID, TLVar, Idx, SingletonGlobal)
   (8)  shadow.mem.out(NodeID, TLVar, Idx, SingletonGlobal)
   (9)  TLVar shadow.mem.arg.init(NodeID, SingletonGlobal)
   (10) TLVar shadow.mem.global.init(NodeID, TLVar, Global)

   NodeID is a unique identifier (i32) for memory regions.

   LLVM callsites are surrounded by calls to shadow.mem.arg.ref,
   shadow.mem.arg.mod, shadow.mem.arg.ref_mod, and shadow.mem.arg_new
   (3)-(6). These functions provide information whether the memory
   region passed to the callee is read-only, modified,
   read-and-modified, or created by the callee, respectively.

   Functions also contain calls to shadow.mem.in (7) and shadow.mem.out (8)
   (they are always located in the exit blocks of the functions). They
   represent the input and output regions of the function.

   With (3)-(8) we have enough information to match actual and formal
   parameters at callsites. To know which actual needs to be matched
   to which formal we use Idx (i32).

   There are also special functions for initialization of globals and
   functions parameters (9)-(10). If the global variable constains a
   unique scalar then the initialization is done via (9). Otherwise,
   (10) is used.

   TLVar (i32) refers to a top-level variable (i.e., LLVM register) of
   pointer type. Note that some of these functions return a TLVar,
   representing the primed version of the memory region after an
   update occurs. Finally, SingletonGlobal (i8*) denotes whether the
   memory region corresponds to a global variable whose address has
   not been taken.

   An use of memory region is denoted by (1) and (2) denotes an use
   and definition.

   Comparison with SVF (https://github.com/SVF-tools/SVF):

     The effects of ShadowMem are pretty similar to what SVF does
     (https://github.com/SVF-tools/SVF/wiki/Technical-documentation)
     with its memory SSA construction. Our special functions
     shadow.mem.load and shadow.mem.store correspond to SVF's MU and
     CHI functions, respectively. Similary, shadow.mem.arg.XXX,
     shadow.mem.in, and shadow.mem.out correspond roughly to CallMU,
     CallCHI, EntryCHI, and RetMU. However, one advantage of our
     instrumentation is that we don't need special calls for PHI nodes
     (MSSAPHI in SVF). Instead, we can use standard PHI nodes where
     operands are shadow.mem id's.
*/

namespace previrt {
namespace analysis {

enum class ShadowMemOp {
  MEM_SSA_LOAD,     /* load (use) */
  MEM_SSA_STORE,    /* store (definition) */
  MEM_SSA_ARG_INIT, /* initial value of a formal parameter or global with unique
                       scalar */
  MEM_SSA_GLOBAL_INIT, /* initial value of global */
  MEM_SSA_ARG_REF,     /* (read-only) input actual parameter */
  MEM_SSA_ARG_MOD,     /* (modified) input/output actual parameter */
  MEM_SSA_ARG_REF_MOD, /* (read-and-modified) input/output actual parameter */
  MEM_SSA_ARG_NEW,     /* output actual parameter */
  MEM_SSA_FUN_IN,      /* input formal parameter */
  MEM_SSA_FUN_OUT,     /* output formal parameter */
  NON_MEM_SSA
};

inline llvm::raw_ostream &operator<<(llvm::raw_ostream &o, ShadowMemOp op) {
  switch (op) {
  case ShadowMemOp::MEM_SSA_LOAD:
    o << "shadow.mem.load";
    break;
  case ShadowMemOp::MEM_SSA_STORE:
    o << "shadow.mem.store";
    break;
  case ShadowMemOp::MEM_SSA_ARG_INIT:
    o << "shadow.mem.arg.init";
    break;
  case ShadowMemOp::MEM_SSA_GLOBAL_INIT:
    o << "shadow.mem.global.init";
    break;
  case ShadowMemOp::MEM_SSA_ARG_REF:
    o << "shadow.mem.arg.ref";
    break;
  case ShadowMemOp::MEM_SSA_ARG_MOD:
    o << "shadow.mem.arg.mod";
    break;
  case ShadowMemOp::MEM_SSA_ARG_REF_MOD:
    o << "shadow.mem.arg.ref_mod";
    break;
  case ShadowMemOp::MEM_SSA_ARG_NEW:
    o << "shadow.mem.arg.new";
    break;
  case ShadowMemOp::MEM_SSA_FUN_IN:
    o << "shadow.mem.arg.in";
    break;
  case ShadowMemOp::MEM_SSA_FUN_OUT:
    o << "shadow.mem.arg.out";
    break;
  default:
    o << "Unexpected shadow mem op";
  }
  return o;
}

inline ShadowMemOp shadowMemStrToOp(llvm::StringRef name) {
  if (name.equals("shadow.mem.load"))
    return ShadowMemOp::MEM_SSA_LOAD;
  if (name.equals("shadow.mem.store"))
    return ShadowMemOp::MEM_SSA_STORE;
  if (name.equals("shadow.mem.arg.init"))
    return ShadowMemOp::MEM_SSA_ARG_INIT;
  if (name.equals("shadow.mem.global.init"))
    return ShadowMemOp::MEM_SSA_GLOBAL_INIT;
  if (name.equals("shadow.mem.arg.ref"))
    return ShadowMemOp::MEM_SSA_ARG_REF;
  if (name.equals("shadow.mem.arg.mod"))
    return ShadowMemOp::MEM_SSA_ARG_MOD;
  if (name.equals("shadow.mem.arg.ref_mod"))
    return ShadowMemOp::MEM_SSA_ARG_REF_MOD;
  if (name.equals("shadow.mem.arg.new"))
    return ShadowMemOp::MEM_SSA_ARG_NEW;
  if (name.equals("shadow.mem.in"))
    return ShadowMemOp::MEM_SSA_FUN_IN;
  if (name.equals("shadow.mem.out"))
    return ShadowMemOp::MEM_SSA_FUN_OUT;
  return ShadowMemOp::NON_MEM_SSA;
}

// Return the "singleton" field from a memory ssa operation
inline const llvm::Value *getShadowMemSingleton(const llvm::CallBase *CB,
                                                ShadowMemOp op) {
  if (!CB)
    return nullptr;
  switch (op) {
  case ShadowMemOp::MEM_SSA_LOAD:
  case ShadowMemOp::MEM_SSA_STORE:
    return CB->getArgOperand(2)->stripPointerCasts();
  case ShadowMemOp::MEM_SSA_ARG_INIT:
    return CB->getArgOperand(1)->stripPointerCasts();
  case ShadowMemOp::MEM_SSA_ARG_REF:
  case ShadowMemOp::MEM_SSA_ARG_MOD:
  case ShadowMemOp::MEM_SSA_ARG_REF_MOD:
  case ShadowMemOp::MEM_SSA_ARG_NEW:
  case ShadowMemOp::MEM_SSA_FUN_IN:
  case ShadowMemOp::MEM_SSA_FUN_OUT:
    return CB->getArgOperand(3)->stripPointerCasts();
  default:
    return nullptr;
  }
}

#define DeclareIsShadowMem(Name, Op)                                          \
  inline bool isShadowMem##Name(const llvm::CallBase *CB,                     \
                                bool onlySingleton) {                         \
    if (!CB || !CB->getCalledFunction())                                      \
      return false;                                                           \
    if (shadowMemStrToOp(CB->getCalledFunction()->getName()) == (Op)) {        \
      if (!onlySingleton ||                                                   \
          !llvm::isa<llvm::ConstantPointerNull>(                              \
              getShadowMemSingleton(CB, Op))) {                               \
        return true;                                                          \
      }                                                                       \
    }                                                                         \
    return false;                                                             \
  }                                                                           \
  inline bool isShadowMem##Name(const llvm::Instruction *I,                   \
                                bool onlySingleton) {                         \
    if (const llvm::CallBase *CB = llvm::dyn_cast<const llvm::CallBase>(I)) { \
      return isShadowMem##Name(CB, onlySingleton);                            \
    }                                                                         \
    return false;                                                             \
  }

// isShadowMemLoad
DeclareIsShadowMem(Load, ShadowMemOp::MEM_SSA_LOAD)
// isShadowMemStore
DeclareIsShadowMem(Store, ShadowMemOp::MEM_SSA_STORE)
// isShadowMemArgInit
DeclareIsShadowMem(ArgInit, ShadowMemOp::MEM_SSA_ARG_INIT)
// isShadowMemGlobalInit
DeclareIsShadowMem(GlobalInit, ShadowMemOp::MEM_SSA_GLOBAL_INIT)
// isShadowMemArgRef
DeclareIsShadowMem(ArgRef, ShadowMemOp::MEM_SSA_ARG_REF)
// isShadowMemArgMod
DeclareIsShadowMem(ArgMod, ShadowMemOp::MEM_SSA_ARG_MOD)
// isShadowMemArgRefMod
DeclareIsShadowMem(ArgRefMod, ShadowMemOp::MEM_SSA_ARG_REF_MOD)
// isShadowMemArgNew
DeclareIsShadowMem(ArgNew, ShadowMemOp::MEM_SSA_ARG_NEW)
// isShadowMemFunIn
DeclareIsShadowMem(FunIn, ShadowMemOp::MEM_SSA_FUN_IN)
// isShadowMemFunOut
DeclareIsShadowMem(FunOut, ShadowMemOp::MEM_SSA_FUN_OUT)

#undef DeclareIsShadowMem

// Return the "index" field from memory SSA formal or actual parameters.
inline int64_t getShadowMemParamIdx(const llvm::CallBase *CB) {
  int64_t idx = -1;
  if (CB && CB->getCalledFunction() &&
      (isShadowMemArgRef(CB, false /*onlySingleton*/) ||
       isShadowMemArgMod(CB, false /*onlySingleton*/) ||
       isShadowMemArgRefMod(CB, false /*onlySingleton*/) ||
       isShadowMemArgNew(CB, false /*onlySingleton*/) ||
       isShadowMemFunIn(CB, false /*onlySingleton*/) ||
       isShadowMemFunOut(CB, false /*onlySingleton*/))) {
    const llvm::Value *arg = CB->getArgOperand(2);
    if (const llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(arg)) {
      idx = CI->getSExtValue();
    }
  }
  return idx;
}

/* Return the non-primed and primed fields from memory ssa actual
   parameters. For instance, given
      %6 = call i32 @shadow.mem.arg.mod(i32 12, i32 %2, i32 1, ...)
   the non-primed variable is %2 and the primed one is %6.
*/
inline const llvm::Value *getShadowMemParamNonPrimed(const llvm::CallBase *CB,
                                                  bool onlySingleton) {
  if (CB && CB->getCalledFunction() &&
      (isShadowMemArgRef(CB, onlySingleton) || isShadowMemArgMod(CB, onlySingleton) ||
       isShadowMemArgRefMod(CB, onlySingleton) ||
       isShadowMemArgNew(CB, onlySingleton))) {
    return CB->getArgOperand(1);
  } else {
    return nullptr;
  }
}

inline const llvm::Value *getShadowMemParamPrimed(const llvm::CallBase *CB,
                                               bool onlySingleton) {
  if (CB && CB->getCalledFunction() &&
      (isShadowMemArgMod(CB, onlySingleton) ||
       isShadowMemArgRefMod(CB, onlySingleton) ||
       isShadowMemArgNew(CB, onlySingleton))) {
    return CB;
  } else {
    return nullptr;
  }
}

// Linear on the number of uses.
inline bool hasShadowMemLoadUser(const llvm::Value *V, bool onlySingleton) {
  for (auto const &use : V->uses()) {
    if (const llvm::Instruction *I =
            llvm::dyn_cast<const llvm::Instruction>(use.getUser())) {
      if (isShadowMemLoad(I, onlySingleton)) {
        return true;
      }
    }
  }
  return false;
}

/* Given a LLVM callsite c, the memory SSA form will produce one c's
 * actual argument per memory region accessed by the callee. To
 * avoid conflicting with the original LLVM callsite, each c's
 * actual parameter is identified by extra callsites executing prior
 * to c to these special functions:
 *
 * - shadow.mem.arg_ref
 * - shadow.mem.arg_mod
 * - shadow.mem.arg_ref_mod
 * - shadow.mem.arg_new
 *
 * For instance, given an original LLVM callsite:
 *   %y = call i32 @foo(%x)
 * if its memory SSA form looks like this:
 *   %5 = call i32 @shadow.mem.arg.ref_mod(i32 8, i32 %4, i32 0, ...)
 *   %6 = call i32 @shadow.mem.arg.mod(i32 12, i32 %3, i32 1, ...)
 *   %y = call i32 @foo(%x)
 *
 * We know that foo accesses two memory regions denoted by
 * identifiers 8 and 12. The first one is read and modified and the
 * second is only modified. Since they are both modified the memory
 * SSA form also indicates the non-primed (primed) names before
 * (after) the region is modified. E.g., for the region 8 the
 * variable %4 represents the region before the update and %5 is the
 * region after the update.
 */
class ShadowMemSSACallSite {

  llvm::CallBase *m_ci;
  std::vector<const llvm::CallBase *> m_actual_params;
  bool m_only_singleton;

public:
  ShadowMemSSACallSite(llvm::CallBase *ci, bool only_singleton);

  // Return number of memory-related actual parameters
  unsigned numParams() const { return m_actual_params.size(); }

  // return true if the shadow.mem.XXX instruction associated with the
  // idx-th actual parameter is shadow.mem.arg_ref and it corresponds
  // to a singleton memory region if m_only_singleton is true.
  bool isRef(unsigned idx) const;

  // return true if the shadow.mem.XXX instruction associated with the
  // idx-th actual parameter is shadow.mem.arg_mod and it corresponds
  // to a singleton memory region if m_only_singleton is true.
  bool isMod(unsigned idx) const;

  // return true if the shadow.mem.XXX instruction associated with the
  // idx-th actual parameter is shadow.mem.arg_ref_mod and it
  // corresponds to a singleton memory region if m_only_singleton is
  // true.
  bool isRefMod(unsigned idx) const;

  // return true if the shadow.mem.XXX instruction associated with the
  // idx-th actual parameter is shadow.mem.arg_new and it corresponds
  // to a singleton memory region if m_only_singleton is true.
  bool isNew(unsigned idx) const;

  // return the non-primed top-level variable of the shadow.mem.XXX
  // instruction associated with the idx-th actual parameter.
  const llvm::Value *getNonPrimed(unsigned idx) const;

  // return the primed top-level variable of the shadow.mem.XXX
  // instruction associated with the idx-th actual parameter.
  const llvm::Value *getPrimed(unsigned idx) const;

  void write(llvm::raw_ostream &o) const;

  void dump() const;
};

/*
  Gather memory SSA-related input/output formal parameters of a
  function.
*/
class ShadowMemSSAFunction {
  llvm::Function &m_F;
  // map from index to Value*
  // Important: cannot use a vector because the indexes are not
  // necessarily consecutive.
  std::map<unsigned, const llvm::Value *> m_in_formal_params;
  std::map<unsigned, const llvm::CallBase *> m_out_formal_params;
  bool m_only_singleton;

public:
  ShadowMemSSAFunction(llvm::Function &F, llvm::Pass &P, bool only_singleton);

  // Return value can be null if not found
  const llvm::Value *getInFormal(unsigned idx) const;
  const llvm::CallBase *getOutFormal(unsigned idx) const;

  unsigned getNumInFormals() const { return m_in_formal_params.size(); }
  unsigned getNumOutFormals() const { return m_out_formal_params.size(); }
};

/*
   Gather memory SSA-related information about functions and
   callsites for queries.
*/
class ShadowMemSSACallsManager {
  llvm::Module &m_M;
  llvm::DenseMap<const llvm::CallBase *, ShadowMemSSACallSite *> m_callsites;
  llvm::DenseMap<const llvm::Function *, ShadowMemSSAFunction *> m_functions;
  bool m_only_singleton;

public:
  ShadowMemSSACallsManager(llvm::Module &M, llvm::Pass &P, bool only_singleton);

  ~ShadowMemSSACallsManager();

  const ShadowMemSSAFunction *getFunction(const llvm::Function *F) const;

  const ShadowMemSSACallSite *getCallSite(const llvm::CallBase *CI) const;
};
} // namespace analysis
} // namespace previrt
