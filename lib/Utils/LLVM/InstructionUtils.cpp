#include "Utils/LLVM/InstructionUtils.h"

#include "Utils/LLVM/Demangle.h"

#include <algorithm>
#include <set>
#include <unordered_set>

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Statepoint.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Path.h>

using namespace llvm;

// Returns true if the instruction operates on pointers.
bool InstructionUtils::isPointerInstruction(Instruction *I) {
  return isa<LoadInst>(I) || isa<StoreInst>(I) || isa<VAArgInst>(I);
}

// Returns the line number of the instruction from debug info.
unsigned InstructionUtils::getLineNumber(Instruction &I) {
  const DebugLoc &DL = I.getDebugLoc();
  return DL ? DL.getLine() : -1;
}

// Returns the name of the instruction or "No Name" if unnamed.
std::string InstructionUtils::getInstructionName(Instruction *I) {
  return I->hasName() ? DemangleUtils::demangleWithCleanup(I->getName().str())
                      : "No Name";
}

// Returns the name of the value or "No Name" if unnamed.
std::string InstructionUtils::getValueName(Value *v) {
  return v->hasName() ? DemangleUtils::demangleWithCleanup(v->getName().str())
                      : "No Name";
}

// Escapes special characters in a string for JSON output.
std::string InstructionUtils::escapeJsonString(const std::string &input) {
  std::ostringstream ss;
  for (std::string::const_iterator iter = input.begin(); iter != input.end();
       iter++) {
    switch (*iter) {
    case '\\':
      ss << "\\\\";
      break;
    case '"':
      ss << "\\\"";
      break;
    case '/':
      ss << "\\/";
      break;
    case '\b':
      ss << "\\b";
      break;
    case '\f':
      ss << "\\f";
      break;
    case '\n':
      ss << "\\n";
      break;
    case '\r':
      ss << "\\r";
      break;
    case '\t':
      ss << "\\t";
      break;
    default:
      ss << *iter;
      break;
    }
  }
  return ss.str();
}

// Escapes a Value as a string for JSON output.
std::string InstructionUtils::escapeValueString(Value *currInstr) {
  std::string str;
  llvm::raw_string_ostream rso(str);
  currInstr->print(rso);
  return escapeJsonString(rso.str());
}

// Recursively searches for debug location in predecessor blocks.
DILocation *getRecursiveDILoc(Instruction *I, std::string &fileName,
                              std::set<BasicBlock *> &visited) {
  DILocation *DL = I->getDebugLoc().get();
  if (fileName.empty())
    return DL;
  if (DL && DL->getFilename().equals(fileName))
    return DL;

  BasicBlock *BB = I->getParent();
  if (!visited.insert(BB).second)
    return nullptr;

  for (auto &Inst : BB->getInstList()) {
    DILocation *InstDL = Inst.getDebugLoc();
    if (InstDL && InstDL->getFilename().equals(fileName))
      return InstDL;
    if (&Inst == I)
      break;
  }

  for (BasicBlock *Pred : predecessors(BB)) {
    if (DILocation *PredDL =
            getRecursiveDILoc(Pred->getTerminator(), fileName, visited))
      return PredDL;
  }
  return nullptr;
}

// Returns the filename associated with the function from debug info.
std::string getFunctionFileName(Function *F) {
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;
  F->getAllMetadata(MDs);
  for (auto &MD : MDs) {
    if (MDNode *N = MD.second) {
      if (auto *subProgram = dyn_cast<DISubprogram>(N)) {
        return subProgram->getFilename().str();
      }
    }
  }
  return "";
}

// Returns the correct debug location for an instruction.
DILocation *InstructionUtils::getCorrectInstrLocation(Instruction *I) {
  DILocation *DL = I->getDebugLoc().get();
  if (DL && DL->getFilename().endswith(".c"))
    return DL;

  std::string funcFileName = getFunctionFileName(I->getFunction());
  if (DL && (funcFileName.empty() || DL->getFilename().equals(funcFileName)))
    return DL;

  // Instruction is from an inlined function
  std::set<BasicBlock *> visited;
  if (DILocation *actualLoc = getRecursiveDILoc(I, funcFileName, visited))
    return actualLoc;

  return DL;
}

// Returns the line number for an instruction using correct location.
int InstructionUtils::getInstrLineNumber(Instruction *I) {
  DILocation *DL = getCorrectInstrLocation(I);
  return DL ? DL->getLine() : -1;
}

// ============================================================================
// New utility functions integrated from external source
// ============================================================================

// Static constant for known memory functions
static const std::unordered_set<std::string> KnownMemFuncs = {
    "malloc",
    "calloc",
    "realloc",
    "reallocarray",
    "memalign",
    "aligned_alloc",
    "valloc",
    "pvalloc",
    "posix_memalign",
    "mmap",
    "mmap64",
    "free",
    "free_sized",
    "free_aligned_size",
    "munmap",
    "strdup",
    "strndup",
    "asprintf",
    "aswprintf",
    "vasprintf",
    "vaswprintf",
    "getline",
    "getwline",
    "getdelim",
    "getwdelim",
    "allocate_at_least",
    "construct_at",
    "destroy_at",
    "destroy",
    "destroy_n",
    "new",
    "new[]",
    "delete",
    "delete[]",
    "tempnam",
    "get_current_dir_name",
    "realpath",
    "longjmp",
    "siglongjmp",
    "wcsdup",
    "wcsndup",
    "mbsdup",
    "mbsndup"};

// Helper function to check if character is newline
static inline bool isNewLine(char C) { return C == '\n'; }

// Helper function to truncate and clean string
static inline std::string truncateString(std::string S) {
  std::replace_if(S.begin(), S.end(), isNewLine, '\t');
  return S.substr(0, 128);
}

// Convert LLVM Value to printable string
std::string InstructionUtils::valueToString(const Value *V) {
  std::string S;
  llvm::raw_string_ostream RSO(S);
  if (V)
    V->print(RSO, true);
  return truncateString(S);
}

// Convert LLVM Type to printable string
std::string InstructionUtils::typeToString(const Type *T) {
  std::string S;
  llvm::raw_string_ostream RSO(S);
  if (T)
    T->print(RSO, true);
  return truncateString(S);
}

// Check if instruction is system-defined (not user code)
bool InstructionUtils::isSystemDefined(const Instruction *I) {
  if (!I)
    return true;
  if (auto *Sub = I->getFunction()->getSubprogram())
    return !Sub->getDirectory().startswith_insensitive("/home");
  return true;
}

// Get fully inlined source location
DILocation *InstructionUtils::getFullyInlinedSrcLoc(const Instruction *I) {
  if (!I)
    return nullptr;
  auto *DILoc = I->getDebugLoc().get();
  while (DILoc && DILoc->getInlinedAt())
    DILoc = DILoc->getInlinedAt();
  return DILoc;
}

// Get source location as row and column
std::pair<int64_t, int64_t>
InstructionUtils::getSourceLocation(const Instruction *I) {
  if (!I)
    return {-7, -7};
  DiagnosticLocation Loc(I->getDebugLoc());
  if (!Loc.isValid())
    return {-8, -8};
  return {Loc.getLine() ? (int64_t)Loc.getLine() : -9,
          Loc.getColumn() ? (int64_t)Loc.getColumn() : -9};
}

// Get source location string for a function
std::string
InstructionUtils::getSourceLocationString(const Function *F,
                                          const std::string &ModID) {
  DiagnosticLocation Loc(F ? F->getSubprogram() : nullptr);
  std::string FuncName =
      F ? DemangleUtils::demangle(F->getName().str()) : "unknown_function";
  std::string FileName =
      Loc.isValid() ? llvm::sys::path::filename(Loc.getAbsolutePath()).str()
                    : "unknown_file";
  return FuncName + ";;" + FileName + ";;" + ModID;
}

// Check if instruction is lifetime.start
bool InstructionUtils::isLifetimeStart(const Instruction *I) {
  if (auto *II = dyn_cast_or_null<IntrinsicInst>(I))
    return II->getIntrinsicID() == Intrinsic::lifetime_start;
  return false;
}

// Check if instruction is lifetime.end
bool InstructionUtils::isLifetimeEnd(const Instruction *I) {
  if (auto *II = dyn_cast_or_null<IntrinsicInst>(I))
    return II->getIntrinsicID() == Intrinsic::lifetime_end;
  return false;
}

// Get all alloca instructions for a lifetime marker
std::set<AllocaInst *> InstructionUtils::getAllocas(const IntrinsicInst *II) {
  std::set<AllocaInst *> Allocas;
  if (!II || !II->isLifetimeStartOrEnd())
    return Allocas;

  SmallVector<Value *, 4> SrcObjs;
  getUnderlyingObjectsForCodeGen(II->getOperand(1), SrcObjs);
  for (auto *Obj : SrcObjs)
    if (auto *AI = dyn_cast<AllocaInst>(Obj))
      Allocas.insert(AI);
  return Allocas;
}

// Check if address stays dead after lifetime.end
bool InstructionUtils::staysDead(IntrinsicInst *II) {
  if (!II || !isLifetimeEnd(II))
    return false;

  DominatorTree DTree(*II->getFunction());
  LoopInfo LInfo(DTree);
  SmallPtrSet<Value *, 4> Visited;
  SmallVector<Instruction *, 4> Worklist;

  auto AddWork = [&](Instruction *I) {
    if (I && I != II && Visited.insert(I).second)
      Worklist.push_back(I);
  };

  for (auto *AI : getAllocas(II))
    AddWork(AI);

  while (!Worklist.empty()) {
    auto *I = Worklist.pop_back_val();
    if (!I || I == II)
      continue;
    if (isLifetimeStart(I) &&
        isPotentiallyReachable(I, II, nullptr, &DTree, &LInfo))
      return false;
    for (auto *U : I->users())
      if (isa<Instruction>(U))
        AddWork(cast<Instruction>(U));
  }
  return true;
}

// Get called function (handles statepoints)
Function *InstructionUtils::getCalledFunction(const CallBase *CB) {
  if (!CB)
    return nullptr;
  if (auto *GCSP = dyn_cast<GCStatepointInst>(CB))
    return GCSP->getActualCalledFunction() ?: CB->getCalledFunction();
  return CB->getCalledFunction();
}

// Check if call is possibly unsafe
bool InstructionUtils::isPossiblyUnsafe(const CallBase *CB) {
  return !CB || CB->hasRetAttr(Attribute::NoAlias) ||
         CB->hasFnAttr(Attribute::NoAlias) ||
         !CB->hasRetAttr(Attribute::NoFree) ||
         !CB->hasFnAttr(Attribute::NoFree) || !CB->doesNotAccessMemory() ||
         !CB->returnDoesNotAlias() || CB->mayReadOrWriteMemory() ||
         CB->mayHaveSideEffects() || CB->isIndirectCall() || CB->mayThrow();
}

// Check if function is possibly unsafe
bool InstructionUtils::isPossiblyUnsafe(Function *F,
                                        FunctionAnalysisManager *FAM) {
  if (!F || F->hasFnAttribute(Attribute::InaccessibleMemOnly) ||
      F->hasFnAttribute(Attribute::ReadNone) ||
      F->hasFnAttribute(Attribute::NoAlias) ||
      !F->hasFnAttribute(Attribute::NoFree) ||
      !F->callsFunctionThatReturnsTwice() ||
      KnownMemFuncs.count(DemangleUtils::demangle(F->getName().str())))
    return true;

  if (FAM) {
    auto &TLI = FAM->getResult<TargetLibraryAnalysis>(*F);
    LibFunc TLIF;
    if (isAllocationFn(F, &TLI) || (TLI.getLibFunc(*F, TLIF) && TLI.has(TLIF) &&
                                    isLibFreeFunction(F, TLIF)))
      return true;
  }
  return false;
}

// Check if memory access is fully safe
bool InstructionUtils::isFullySafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis,
                                         Value *Addr, uint64_t TySize) {
  SizeOffsetType SizeOffset = ObjSizeVis.compute(Addr);
  if (!ObjSizeVis.bothKnown(SizeOffset))
    return false;
  uint64_t Size = SizeOffset.first.getZExtValue();
  int64_t Offset = SizeOffset.second.getSExtValue();
  return Offset >= 0 && Size >= uint64_t(Offset) &&
         Size - uint64_t(Offset) >= TySize / 8;
}

// Get known memory functions
const std::unordered_set<std::string> &
InstructionUtils::getKnownMemoryFunctions() {
  return KnownMemFuncs;
}
