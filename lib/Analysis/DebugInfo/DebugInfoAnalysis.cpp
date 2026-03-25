// Use debug info of LLVM to better report bugs, e.g., line number, position,
// function name, variable name, etc.

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include "Utils/LLVM/Demangle.h"

#include <fstream>
#include <limits>
#include <mutex>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <unistd.h>

using namespace llvm;

// Static cache for source file contents (guarded by a mutex for thread safety)
std::map<std::string, std::vector<std::string>>
    DebugInfoAnalysis::sourceFileCache;
static std::mutex sourceFileCacheMutex;

// Maximum cache size (number of files) to bound memory usage
static constexpr size_t MAX_CACHE_FILES = 256;

DebugInfoAnalysis::DebugInfoAnalysis() = default;

//===----------------------------------------------------------------------===//
// Helper Functions (inspired by Phasar)
//===----------------------------------------------------------------------===//

// Strip all pointer casts (BitCastInst, AddrSpaceCastInst) to reach the
// underlying alloca or value.  Returns the original pointer if no cast found.
static const Value *stripAllCasts(const Value *V) {
  while (V) {
    if (const auto *BC = dyn_cast<BitCastInst>(V)) {
      V = BC->getOperand(0);
    } else if (const auto *AC = dyn_cast<AddrSpaceCastInst>(V)) {
      V = AC->getOperand(0);
    } else {
      break;
    }
  }
  return V;
}

// Get ALL DbgVariableIntrinsics for a value (handles ValueAsMetadata and
// Arguments).  Returns a vector so callers can inspect all debug records
// rather than only the first one.
static SmallVector<DbgVariableIntrinsic *, 4>
getDbgVarIntrinsics(const Value *V) {
  SmallVector<DbgVariableIntrinsic *, 4> result;

  // Try ValueAsMetadata approach (more robust)
  if (auto *VAM = ValueAsMetadata::getIfExists(const_cast<Value *>(V))) {
    if (auto *MDV = MetadataAsValue::getIfExists(V->getContext(), VAM)) {
      for (auto *U : MDV->users()) {
        if (auto *DBGIntr = dyn_cast<DbgVariableIntrinsic>(U)) {
          result.push_back(DBGIntr);
        }
      }
    }
  }

  // Handle Arguments: if mem2reg is not activated, formal parameters will be
  // stored in registers at the beginning of function call. Debug info will be
  // linked to those alloca's instead of the arguments itself.
  if (result.empty()) {
    if (const auto *Arg = dyn_cast<Argument>(V)) {
      for (const auto *User : Arg->users()) {
        if (const auto *Store = dyn_cast<StoreInst>(User)) {
          if (Store->getValueOperand() == Arg &&
              isa<AllocaInst>(Store->getPointerOperand())) {
            auto nested = getDbgVarIntrinsics(Store->getPointerOperand());
            result.append(nested.begin(), nested.end());
          }
        }
      }
    }
  }

  return result;
}

// Convenience wrapper: return the first DbgVariableIntrinsic (or nullptr).
static DbgVariableIntrinsic *getDbgVarIntrinsic(const Value *V) {
  auto vec = getDbgVarIntrinsics(V);
  return vec.empty() ? nullptr : vec.front();
}

// Get DILocalVariable from a Value (prefers dbg.declare over dbg.value)
static DILocalVariable *getDILocalVariable(const Value *V) {
  for (auto *DbgIntr : getDbgVarIntrinsics(V)) {
    if (auto *DDI = dyn_cast<DbgDeclareInst>(DbgIntr)) {
      return DDI->getVariable();
    }
  }
  // Fall back to dbg.value if no dbg.declare found
  for (auto *DbgIntr : getDbgVarIntrinsics(V)) {
    if (auto *DVI = dyn_cast<DbgValueInst>(DbgIntr)) {
      return DVI->getVariable();
    }
  }
  return nullptr;
}

// Get DIGlobalVariable from a Value
static DIGlobalVariable *getDIGlobalVariable(const Value *V) {
  if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
    if (auto *MN = GV->getMetadata(LLVMContext::MD_dbg)) {
      if (auto *DIGVExp = dyn_cast<DIGlobalVariableExpression>(MN)) {
        return DIGVExp->getVariable();
      }
    }
  }
  return nullptr;
}

// Get DILocation from a Value
static DILocation *getDILocation(const Value *V) {
  // Arguments and Instructions such as AllocaInst
  if (const auto *I = dyn_cast<Instruction>(V)) {
    if (auto *MN = I->getMetadata(LLVMContext::MD_dbg)) {
      return dyn_cast<DILocation>(MN);
    }
  }

  if (auto *DbgIntr = getDbgVarIntrinsic(V)) {
    if (auto *MN = DbgIntr->getMetadata(LLVMContext::MD_dbg)) {
      return dyn_cast<DILocation>(MN);
    }
  }

  return nullptr;
}

// Get DIFile from a Value
static const DIFile *getDIFileFromIR(const Value *V) {
  if (const auto *GO = dyn_cast<GlobalObject>(V)) {
    if (auto *MN = GO->getMetadata(LLVMContext::MD_dbg)) {
      if (auto *Subpr = dyn_cast<DISubprogram>(MN)) {
        return Subpr->getFile();
      }
      if (auto *GVExpr = dyn_cast<DIGlobalVariableExpression>(MN)) {
        return GVExpr->getVariable()->getFile();
      }
    }
  } else if (const auto *Arg = dyn_cast<Argument>(V)) {
    if (auto *LocVar = getDILocalVariable(Arg)) {
      return LocVar->getFile();
    }
  } else if (const auto *I = dyn_cast<Instruction>(V)) {
    if (I->isUsedByMetadata()) {
      if (auto *LocVar = getDILocalVariable(I)) {
        return LocVar->getFile();
      }
    } else if (I->getMetadata(LLVMContext::MD_dbg)) {
      if (auto *DL = I->getDebugLoc().get()) {
        return DL->getFile();
      }
    }
    if (const auto *DIFun = I->getFunction()->getSubprogram()) {
      return DIFun->getFile();
    }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Source File Handling (adapted from prior implementation)
//===----------------------------------------------------------------------===//

bool DebugInfoAnalysis::loadSourceFile(const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    return false;
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    // Trim leading whitespace for better display
    size_t start = line.find_first_not_of(" \t");
    if (start != std::string::npos) {
      lines.push_back(line.substr(start));
    } else {
      lines.push_back("");
    }
  }
  file.close();

  // Evict oldest entry if cache is full to bound memory usage
  if (sourceFileCache.size() >= MAX_CACHE_FILES) {
    sourceFileCache.erase(sourceFileCache.begin());
  }

  sourceFileCache[filepath] = std::move(lines);
  return true;
}

std::string DebugInfoAnalysis::findSourceFile(const std::string &filename) {
  if (filename.empty()) {
    return "";
  }

  // Use LLVM's file system utilities for better path handling
  if (sys::fs::exists(filename) && !sys::fs::is_directory(filename)) {
    return filename;
  }

  // For absolute paths that don't exist, don't try relative heuristics
  if (sys::path::has_root_directory(filename)) {
    return "";
  }

  // Relative path: try against current working directory and a few parents
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) == nullptr) {
    return "";
  }
  std::string cwdStr(cwd);

  // Build candidate paths using LLVM path utilities
  SmallVector<SmallString<256>, 8> candidates;

  auto makeCandidate = [&](std::initializer_list<StringRef> parts) {
    SmallString<256> buf;
    for (auto &p : parts) {
      sys::path::append(buf, p);
    }
    candidates.push_back(buf);
  };

  makeCandidate({cwdStr, filename});
  makeCandidate({cwdStr, "..", filename});
  makeCandidate({cwdStr, "..", "..", filename});
  makeCandidate({cwdStr, "..", "..", "..", filename});
  makeCandidate({cwdStr, "benchmarks", filename});
  makeCandidate({cwdStr, "..", "benchmarks", filename});

  for (const auto &path : candidates) {
    if (sys::fs::exists(path) && !sys::fs::is_directory(path)) {
      return path.str().str();
    }
  }

  return ""; // Not found
}

//===----------------------------------------------------------------------===//
// Source Code Extraction
//===----------------------------------------------------------------------===//

std::string DebugInfoAnalysis::getSourceCodeStatement(const Instruction *I) {
  if (!I)
    return "";

  std::string filepath = getSourceFile(I);
  int line = getSourceLine(I);

  if (filepath.empty() || line <= 0) {
    return "";
  }

  // Try to find the actual file path
  std::string actualPath = findSourceFile(filepath);
  if (actualPath.empty()) {
    actualPath = filepath; // Use original if not found
  }

  // Check if file exists and is not a directory
  if (!sys::fs::exists(actualPath) || sys::fs::is_directory(actualPath)) {
    return "";
  }

  std::lock_guard<std::mutex> lock(sourceFileCacheMutex);

  // Check cache first
  auto it = sourceFileCache.find(actualPath);
  if (it == sourceFileCache.end()) {
    // Load entire file into cache so subsequent accesses are O(1)
    if (!loadSourceFile(actualPath)) {
      // Cache an empty sentinel so we don't retry on every call
      sourceFileCache[actualPath] = std::vector<std::string>();
      return "";
    }
    it = sourceFileCache.find(actualPath);
  }

  // Get the line from cache (line numbers are 1-based)
  if (it != sourceFileCache.end() && line > 0 &&
      line <= static_cast<int>(it->second.size())) {
    return it->second[static_cast<size_t>(line) - 1];
  }

  return "";
}

//===----------------------------------------------------------------------===//
// Debug Info Extraction (adapted for LLVM 14+)
//===----------------------------------------------------------------------===//

std::string DebugInfoAnalysis::getSourceFile(const Value *V) {
  if (!V)
    return "";

  // Try to get DIFile from various sources
  const DIFile *DIF = getDIFileFromIR(V);
  if (!DIF) {
    // Fallback: try DILocation
    if (auto *DILoc = getDILocation(V)) {
      DIF = DILoc->getFile();
    }
  }

  if (DIF) {
    auto FileName = DIF->getFilename();
    auto DirName = DIF->getDirectory();

    if (FileName.empty()) {
      return "";
    }

    // Use LLVM path utilities for better cross-platform support
    if (!DirName.empty() && !sys::path::has_root_directory(FileName)) {
      SmallString<256> Buf;
      sys::path::append(Buf, DirName, FileName);
      return Buf.str().str();
    }

    return FileName.str();
  }

  // Fallback: try to get from the subprogram's file (more reliable than
  // Module::getSourceFileName which may be a build-system path or empty)
  if (const auto *F = dyn_cast<Function>(V)) {
    if (auto *SP = F->getSubprogram()) {
      if (auto *File = SP->getFile()) {
        auto FileName = File->getFilename();
        auto DirName = File->getDirectory();
        if (!FileName.empty()) {
          if (!DirName.empty() && !sys::path::has_root_directory(FileName)) {
            SmallString<256> Buf;
            sys::path::append(Buf, DirName, FileName);
            return Buf.str().str();
          }
          return FileName.str();
        }
      }
    }
    return "";
  }
  if (const auto *Arg = dyn_cast<Argument>(V)) {
    return getSourceFile(Arg->getParent());
  }
  if (const auto *I = dyn_cast<Instruction>(V)) {
    return getSourceFile(I->getFunction());
  }

  return "";
}

int DebugInfoAnalysis::getSourceLine(const Value *V) {
  if (!V)
    return 0;

  // Try DILocation first (for Arguments and Instructions)
  if (auto *DILoc = getDILocation(V)) {
    return static_cast<int>(DILoc->getLine());
  }

  // Try DISubprogram (for Functions)
  if (const auto *F = dyn_cast<Function>(V)) {
    if (auto *DISubpr = F->getSubprogram()) {
      return static_cast<int>(DISubpr->getLine());
    }
  }

  // Try DIGlobalVariable (for Globals)
  if (auto *DIGV = getDIGlobalVariable(V)) {
    return static_cast<int>(DIGV->getLine());
  }

  return 0;
}

int DebugInfoAnalysis::getSourceColumn(const Value *V) {
  if (!V)
    return 0;

  // Globals and Functions have no column info, only DILocation has column
  if (auto *DILoc = getDILocation(V)) {
    return static_cast<int>(DILoc->getColumn());
  }

  return 0;
}

std::string DebugInfoAnalysis::getSourceLocation(const Instruction *I) {
  if (!I)
    return "unknown:0";

  const DebugLoc &DL = I->getDebugLoc();
  if (!DL)
    return "unknown:0";

  unsigned Line = DL.getLine();
  unsigned Col = DL.getCol();

  // Walk up the inlined-at chain to find the outermost file scope
  const DILocation *Loc = DL.get();
  while (Loc) {
    if (auto *Scope = Loc->getScope()) {
      if (auto *File = Scope->getFile()) {
        StringRef FileName = File->getFilename();
        if (!FileName.empty()) {
          return FileName.str() + ":" + std::to_string(Line) + ":" +
                 std::to_string(Col);
        }
      }
    }
    // Try the inlined-at location if the current scope has no file
    Loc = Loc->getInlinedAt();
    if (Loc) {
      Line = Loc->getLine();
      Col = Loc->getColumn();
    }
  }

  return "unknown:" + std::to_string(DL.getLine());
}

std::string DebugInfoAnalysis::getFunctionName(const Instruction *I) {
  if (!I)
    return "unknown_function";

  const Function *F = I->getFunction();
  if (!F)
    return "unknown_function";

  std::string funcName;

  // Try to get name from debug info first (real source name)
  if (DISubprogram *Subprogram = F->getSubprogram()) {
    funcName = Subprogram->getName().str();
  } else {
    funcName = F->getName().str();
  }

  // Demangle C++ and Rust function names for better readability
  return DemangleUtils::demangleWithCleanup(funcName);
}

std::string DebugInfoAnalysis::getVariableName(const Value *V,
                                               unsigned recursionDepth) {
  if (!V)
    return "temp_var";

  // Guard against unbounded recursion (e.g., cyclic load chains in IR)
  if (recursionDepth > 8)
    return "temp_var";

  // Check cache first
  auto it = varNameCache.find(V);
  if (it != varNameCache.end()) {
    return it->second;
  }

  std::string varName;

  // Try to get variable name from debug info (improved approach from Phasar)
  // First try DILocalVariable
  if (auto *LocVar = getDILocalVariable(V)) {
    varName = LocVar->getName().str();
  }
  // Then try DIGlobalVariable
  else if (auto *GlobVar = getDIGlobalVariable(V)) {
    varName = GlobVar->getName().str();
  }

  // Fallback: scan function for dbg intrinsics (for cases where
  // getDILocalVariable fails)
  if (varName.empty()) {
    if (auto *I = dyn_cast<Instruction>(V)) {
      const Function *F = I->getFunction();
      if (F) {
        for (const auto &BB : *F) {
          for (const auto &Inst : BB) {
            // Check dbg.declare
            if (auto *DbgDeclare = dyn_cast<DbgDeclareInst>(&Inst)) {
              if (DbgDeclare->getAddress() == V) {
                if (auto *Var = DbgDeclare->getVariable()) {
                  varName = Var->getName().str();
                  break;
                }
              }
            }
            // Check dbg.value
            else if (auto *DbgValue = dyn_cast<DbgValueInst>(&Inst)) {
              if (DbgValue->getValue() == V) {
                if (auto *Var = DbgValue->getVariable()) {
                  varName = Var->getName().str();
                  break;
                }
              }
            }
          }
          if (!varName.empty())
            break;
        }
      }
    }
  }

  // Fallback to LLVM IR name
  if (varName.empty() && V->hasName()) {
    varName = V->getName().str();
    // Demangle if needed
    varName = DemangleUtils::demangleWithCleanup(varName);
  }

  // For load/store, try to get the pointer operand's name.
  // Strip casts before recursing to avoid redundant levels.
  if (varName.empty()) {
    if (auto *LI = dyn_cast<LoadInst>(V)) {
      const Value *Ptr = stripAllCasts(LI->getPointerOperand());
      if (Ptr && Ptr != V) {
        std::string ptrName = getVariableName(Ptr, recursionDepth + 1);
        if (ptrName != "temp_var") {
          varName = ptrName;
        } else if (Ptr->hasName()) {
          varName = Ptr->getName().str();
        }
      }
    } else if (auto *SI = dyn_cast<StoreInst>(V)) {
      const Value *Ptr = stripAllCasts(SI->getPointerOperand());
      if (Ptr && Ptr != V) {
        std::string ptrName = getVariableName(Ptr, recursionDepth + 1);
        if (ptrName != "temp_var") {
          varName = ptrName;
        } else if (Ptr->hasName()) {
          varName = Ptr->getName().str();
        }
      }
    } else if (auto *CI = dyn_cast<CallInst>(V)) {
      // For call instructions, use the function name
      if (const Function *F = CI->getCalledFunction()) {
        varName = F->getName().str();
      }
    }
  }

  if (varName.empty()) {
    varName = "temp_var";
  }

  // Cache the result
  varNameCache[V] = varName;
  return varName;
}

// Public entry point (no depth argument exposed in the header)
std::string DebugInfoAnalysis::getVariableName(const Value *V) {
  return getVariableName(V, 0);
}

std::string DebugInfoAnalysis::getTypeName(const Value *V) {
  if (!V)
    return "unknown_type";

  Type *Ty = V->getType();
  if (!Ty)
    return "unknown_type";

  std::string TypeStr;
  raw_string_ostream RSO(TypeStr);
  Ty->print(RSO);
  return RSO.str();
}

//===----------------------------------------------------------------------===//
// Metadata collection helpers (declared in header, implemented here)
//===----------------------------------------------------------------------===//

void DebugInfoAnalysis::collectMetadata(const Function *F) {
  if (!F)
    return;
  // Pre-populate the varNameCache for all debug intrinsics in the function
  // so that subsequent getVariableName() calls are O(1).
  for (const auto &BB : *F) {
    for (const auto &Inst : BB) {
      if (auto *DDI = dyn_cast<DbgDeclareInst>(&Inst)) {
        if (auto *Addr = DDI->getAddress()) {
          if (auto *Var = DDI->getVariable()) {
            if (varNameCache.find(Addr) == varNameCache.end()) {
              varNameCache[Addr] = Var->getName().str();
            }
          }
        }
      } else if (auto *DVI = dyn_cast<DbgValueInst>(&Inst)) {
        if (auto *Val = DVI->getValue()) {
          if (auto *Var = DVI->getVariable()) {
            if (varNameCache.find(Val) == varNameCache.end()) {
              varNameCache[Val] = Var->getName().str();
            }
          }
        }
      }
    }
  }
}

MDNode *DebugInfoAnalysis::findVarInfoMDNode(const Value *V,
                                             const Function *F) {
  if (!V)
    return nullptr;

  // Try ValueAsMetadata path first
  if (auto *VAM = ValueAsMetadata::getIfExists(const_cast<Value *>(V))) {
    if (auto *MDV = MetadataAsValue::getIfExists(V->getContext(), VAM)) {
      for (auto *U : MDV->users()) {
        if (auto *DBGIntr = dyn_cast<DbgVariableIntrinsic>(U)) {
          return DBGIntr->getVariable();
        }
      }
    }
  }

  // Scan the provided function (or the instruction's parent function)
  const Function *SearchFn = F;
  if (!SearchFn) {
    if (auto *I = dyn_cast<Instruction>(V)) {
      SearchFn = I->getFunction();
    }
  }
  if (!SearchFn)
    return nullptr;

  for (const auto &BB : *SearchFn) {
    for (const auto &Inst : BB) {
      if (auto *DDI = dyn_cast<DbgDeclareInst>(&Inst)) {
        if (DDI->getAddress() == V) {
          return DDI->getVariable();
        }
      } else if (auto *DVI = dyn_cast<DbgValueInst>(&Inst)) {
        if (DVI->getValue() == V) {
          return DVI->getVariable();
        }
      }
    }
  }

  return nullptr;
}
