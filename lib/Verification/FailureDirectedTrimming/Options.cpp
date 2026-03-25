// Command-line options for failure-directed trimming (paper §6 Implementation).
// Instrumentation strategy, bound sizes, and QE/nondet for existentials.

#include "FailureDirectedTrimmingImpl.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

using namespace llvm;

// -----------------------------------------------------------------------------
// Command-line options (paper §6: where to instrument, max conjuncts, QE)
// -----------------------------------------------------------------------------
cl::opt<bool> FDTrimInstrumentCalls(
    "fdtrim-instrument-calls",
    cl::desc("Insert trimming assumes before procedure calls"), cl::init(true));

cl::opt<bool> FDTrimInstrumentConditionals(
    "fdtrim-instrument-conditionals",
    cl::desc("Insert trimming assumes before conditionals"), cl::init(false));

cl::opt<bool>
    FDTrimInstrumentLoops("fdtrim-instrument-loops",
                          cl::desc("Insert trimming assumes at loop headers"),
                          cl::init(true));

cl::opt<unsigned>
    FDTrimMaxConjuncts("fdtrim-max-conjuncts",
                       cl::desc("Maximum number of conjuncts in each inserted "
                                "assume condition (0 = no limit)"),
                       cl::init(0));

cl::opt<unsigned> FDTrimSummaryIterations(
    "fdtrim-summary-iterations",
    cl::desc(
        "Number of summary refinement iterations (0 = compute no summaries)"),
    cl::init(2));

cl::opt<unsigned> FDTrimCFGIterations(
    "fdtrim-cfg-iterations",
    cl::desc("Maximum number of CFG fixpoint iterations per function"),
    cl::init(4));

cl::opt<std::string> FDTrimQuantElim(
    "fdtrim-qe",
    cl::desc("Quantifier elimination for trimming conditions: nondet or z3"),
    cl::init("nondet"));

cl::opt<unsigned> FDTrimQETTimeoutMs(
    "fdtrim-qe-timeout-ms",
    cl::desc("Z3 QE timeout per trimming condition (milliseconds)"),
    cl::init(50));

cl::opt<std::string> FDTrimIntSemantics(
    "fdtrim-int-semantics",
    cl::desc("Integer semantics for Z3-based simplification: bv or math"),
    cl::init("bv"));

cl::opt<std::string> FDTrimDerefMode(
    "fdtrim-deref-mode",
    cl::desc("Codegen for drf(α) terms in assumptions: load, uf, or nondet"),
    cl::init("uf"));

cl::opt<std::string> FDTrimAA(
    "fdtrim-aa",
    cl::desc(
        "Alias analysis backend for trimming (e.g., seadsa, andersen, tpa)"),
    cl::init("seadsa"));

cl::opt<bool>
    FDTrimModelUBOps("fdtrim-model-ub-ops",
                     cl::desc("Model potentially-UB/poisoning integer ops "
                              "(div/rem/shifts) instead of havocing them"),
                     cl::init(false));

// -----------------------------------------------------------------------------
// Name predicates and getVerifierAssume
// -----------------------------------------------------------------------------
bool isAssumeNotFunctionName(StringRef Name) {
  return Name == "verifier.assume.not" || Name == "__VERIFIER_assume_not" ||
         Name == "__CRAB_assume_not";
}

bool isAssumeFunctionName(StringRef Name) {
  return Name == "verifier.assume" || Name == "__VERIFIER_assume" ||
         Name == "__CRAB_assume" || Name == "__SEA_assume" ||
         Name == "llvm.assume";
}

bool isNondetFunctionName(StringRef Name) {
  return Name.startswith("verifier.nondet");
}

bool isAssertFunctionName(StringRef Name) {
  return Name == "__VERIFIER_assert" || Name == "verifier.assert" ||
         Name == "__CRAB_assert" || Name == "__SEA_assert";
}

bool isErrorFunctionName(StringRef Name) {
  return Name == "__VERIFIER_error" || Name == "verifier.error" ||
         Name == "seahorn.error" || Name == "__SEAHORN_error" ||
         Name == "__assert_fail" || Name == "llvm.trap" ||
         Name == "seahorn.fail";
}

FunctionCallee getVerifierAssume(Module &M) {
  LLVMContext &Ctx = M.getContext();
  auto *BoolTy = Type::getInt1Ty(Ctx);
  auto *VoidTy = Type::getVoidTy(Ctx);

  AttrBuilder B(Ctx);
  B.addAttribute(Attribute::NoUnwind);
  B.addAttribute(Attribute::NoRecurse);
  B.addAttribute(Attribute::OptimizeNone);
  B.addAttribute(Attribute::NoInline);
  B.addAttribute(Attribute::InaccessibleMemOnly);

  AttributeList Attrs =
      AttributeList::get(Ctx, AttributeList::FunctionIndex, B);
  return M.getOrInsertFunction("verifier.assume", Attrs, VoidTy, BoolTy);
}

// -----------------------------------------------------------------------------
// NondetFactory / DerefUFFactory
// -----------------------------------------------------------------------------
FunctionCallee NondetFactory::get(Type *Ty) {
  auto It = Cache.find(Ty);
  if (It != Cache.end())
    return It->second;

  std::string Name = ("verifier.nondet.trim." + std::to_string(Cache.size()));
  FunctionCallee Callee = M.getOrInsertFunction(Name, Ty);
  Cache[Ty] = Callee;
  return Callee;
}

Value *NondetFactory::nondet(IRBuilder<> &B, Type *Ty) {
  return B.CreateCall(get(Ty));
}

Value *NondetFactory::nondetBool(IRBuilder<> &B) {
  return B.CreateCall(get(Type::getInt1Ty(M.getContext())));
}

FunctionCallee DerefUFFactory::get(Type *RetTy, unsigned AddrSpace) {
  uint64_t Key =
      llvm::hash_combine(reinterpret_cast<uintptr_t>(RetTy), AddrSpace);
  auto It = Cache.find(Key);
  if (It != Cache.end())
    return It->second;

  LLVMContext &Ctx = M.getContext();
  Type *I8PtrTy = Type::getInt8PtrTy(Ctx, AddrSpace);

  std::string Name = ("verifier.drf.trim." + std::to_string(Cache.size()));

  AttrBuilder B(Ctx);
  B.addAttribute(Attribute::NoUnwind);
  B.addAttribute(Attribute::WillReturn);
  B.addAttribute(Attribute::ReadNone);
  B.addAttribute(Attribute::NoRecurse);

  AttributeList Attrs =
      AttributeList::get(Ctx, AttributeList::FunctionIndex, B);
  FunctionCallee Callee = M.getOrInsertFunction(Name, Attrs, RetTy, I8PtrTy);
  Cache[Key] = Callee;
  return Callee;
}
