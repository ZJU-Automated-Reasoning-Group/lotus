#ifndef _INT_GLOBAL_H
#define _INT_GLOBAL_H

#include "llvm/Support/CommandLine.h"

#include "Alias/TypeQualifier/CRange.h"
#include "Alias/TypeQualifier/Common.h"
#include "Alias/TypeQualifier/FunctionSummary.h"
#include "Alias/TypeQualifier/StructAnalyzer.h"
#include "Alias/TypeQualifier/TaintSignature.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

using CallInstSet = llvm::SmallPtrSet<llvm::CallInst *, 8>;
using CallerMap = llvm::DenseMap<llvm::Function *, CallInstSet>;
using FuncSet = llvm::SmallPtrSet<llvm::Function *, 8>;
using RangeMap = std::map<std::string, CRange>;
using ValueRangeMap = std::map<llvm::Value *, CRange>;
using FuncValueRangeMaps = std::map<llvm::BasicBlock *, ValueRangeMap>;

#if 0
class FuncSet : public FunctionSmallPtrSet
{
public:
	FuncSet() : CallerInst(nullptr), Callers(nullptr) {
		FunctionSmallPtrSet();
	}

	// To easily support backward functionlity, the insert function of Callee
	// set is overriden and Caller set is maintained together on the fly.
	std::pair<FunctionSmallPtrSet::iterator, bool> insert(llvm::Function *F) {
		// Insert Caller information if possible.
		if (CallerInst && Callers) {
			CallInstSet &CIS = Callers->FindAndConstruct(F).second;
			CIS.insert(CallerInst);
		}

		// Insert Callee information.
		return FunctionSmallPtrSet::insert(F);
	}

	void setCallerInfo(llvm::CallInst *_CallerInst, CallerMap *_Callers) {
		CallerInst = _CallerInst;
		Callers = _Callers;
	}

private:
	llvm::CallInst *CallerInst;
	CallerMap *Callers;
};
#endif

typedef std::vector<std::pair<llvm::Module *, llvm::StringRef>> ModuleList;
typedef std::unordered_map<std::string, llvm::Function *> FuncMap;
typedef FuncMap NameFuncMap; // Alias for FuncMap
typedef std::unordered_map<std::string, llvm::GlobalVariable *> GObjMap;
typedef std::unordered_map<std::string, FuncSet> FuncPtrMap;
typedef llvm::DenseMap<llvm::CallInst *, FuncSet> CalleeMap;

// Forward declarations
typedef std::set<std::string> DescSet;
class TaintInstLogger;

class TaintMap {

public:
  typedef std::map<std::string, std::pair<DescSet, bool>> GlobalMap;
  typedef std::map<llvm::Value *, DescSet> ValueMap;

  GlobalMap GTS;
  ValueMap VTS;

  void add(llvm::Value *V, const DescSet &D) {
    VTS[V].insert(D.begin(), D.end());
  }
  void add(llvm::Value *V, llvm::StringRef D) { VTS[V].insert(D.str()); }
  DescSet *get(llvm::Value *V) {
    ValueMap::iterator it = VTS.find(V);
    if (it != VTS.end())
      return &it->second;
    return NULL;
  }

  DescSet *getGlobal(const std::string &ID) {
    if (ID.empty())
      return NULL;
    GlobalMap::iterator it = GTS.find(ID);
    if (it != GTS.end())
      return &it->second.first;
    return NULL;
  }
  bool addGlobalIfNotExist(const std::string &ID, const DescSet &D,
                           TaintInstLogger &Logger, bool isSource = false) {
    if (this->getGlobal(ID))
      return false;

    return this->addGlobal(ID, D, Logger, isSource);
  }

  bool addGlobal(const std::string &ID, const DescSet &D,
                 TaintInstLogger &Logger, bool isSource = false) {
    if (ID.empty())
      return false;
    std::pair<DescSet, bool> &entry = GTS[ID];
    bool isNew = entry.first.empty();

    // Update DescSet.
    for (const auto &DescElem : D) {
      auto res = entry.first.insert(DescElem);
      isNew |= res.second;
    }

    entry.second |= isSource;

    // Logger.addNewTaint(ID, entry.first, isNew, isSource);
    return isNew;
  }
  bool isSource(const std::string &ID) {
    if (ID.empty())
      return false;
    GlobalMap::iterator it = GTS.find(ID);
    if (it == GTS.end())
      return false;
    return it->second.second;
  }
};

typedef std::unordered_map<std::string, int> SimpleTaintMap;
typedef std::set<std::string> SimpleSet;

struct GlobalContext {
  // Map global object name to object definition
  GObjMap Gobjs;

  // Map global function name to function defination
  FuncMap Funcs;

  // Map function pointers (IDs) to possible assignments
  FuncPtrMap FuncPtrs;

  // Map a callsite to all potential callee functions.
  CalleeMap Callees;

  // Map a function to all potential caller instructions.
  CallerMap Callers;

  // Conservative mapping for callees for a callsite
  CalleeMap ConsCallees;

  // Taints
  TaintMap Taints;

  // Ranges
  RangeMap IntRanges;
  FuncValueRangeMaps FuncVRMs;

  // TaintSigs
  std::set<std::string> SStructs;
  std::set<std::string> SArgs;
  std::set<std::string> SVars;

  // Simple Taints and Sinks
  SimpleTaintMap STaints;
  SimpleSet SSources;
  SimpleSet SSinks;

  // StructAnalyzer
  StructAnalyzer structAnalyzer;

  // Additional members for TypeQualifier analysis
  typedef std::map<llvm::Function *, Summary> FuncSummaryMap;
  FuncSummaryMap FSummaries;

  typedef std::unordered_map<llvm::Function *, std::set<llvm::Function *>>
      CallMapType;
  CallMapType CallMaps;
  CallMapType CalledMaps;

  std::set<llvm::Function *> ReadyList;
  std::map<llvm::Function *, bool> Visit;
  std::map<llvm::Function *, int> RemainedFunction;

  typedef std::vector<std::vector<llvm::Function *>> SCCType;
  SCCType SCC;
  std::set<llvm::Function *> indFuncs;

  std::map<llvm::Function *, bool> ChangeMap;

  std::set<std::string> HeapAllocFuncs;
  std::set<std::string> ZeroMallocFuncs;
  std::set<std::string> OtherFuncs;
  std::set<std::string> InitFuncs;
  std::set<std::string> CopyFuncs;
  std::set<std::string> TransferFuncs;
  std::set<std::string> ObjSizeFuncs;
  bool functionModelsInitialized = false;

  std::map<std::string, std::set<llvm::Function *>> nameFuncs;

  ModuleList Modules;
  std::unordered_map<const llvm::StructType *, std::set<int>> usedField;
  bool incAnalysis = false;
  std::string jsonInc;
  std::string jsonfile;
  std::string heapWarning;

  // Function to warnings mapping (function -> map of warning id to message)
  std::map<llvm::Function *, std::map<int, std::string>> fToWarns;
};

class IterativeModulePass {
protected:
  GlobalContext *Ctx;
  const char *ID;

public:
  IterativeModulePass(GlobalContext *Ctx_, const char *ID_)
      : Ctx(Ctx_), ID(ID_) {}

  // run on each module before iterative pass
  virtual bool doInitialization(llvm::Module *M) { return true; }

  // run on each module after iterative pass
  virtual bool doFinalization(llvm::Module *M) { return true; }

  // iterative pass
  virtual bool doModulePass(llvm::Module *M) { return false; }

  virtual void run(ModuleList &modules);
};

#define MD_NAME_INFO_OP 0
#define MD_ARG_INFO_OP 1

// getStringFromMD is defined in Annotation.h

#endif
