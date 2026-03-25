//===- SaberCheckerAPI.h -- API for checkers in Saber-------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef SABERCHECKERAPI_H_
#define SABERCHECKERAPI_H_

#include "IR/ICFG/ICFGNode.h"

#include <map>
#include <string>

#include <llvm/IR/Instructions.h>

namespace lotus {
namespace analysis {

/*
 * Saber Checker API class contains interfaces for various bug checking
 * memory leak detection e.g., alloc free
 * incorrect file operation detection, e.g., fopen, fclose
 */
class SaberCheckerAPI {

public:
  enum CHECKER_TYPE {
    CK_DUMMY = 0, /// dummy type
    CK_ALLOC,     /// memory allocation
    CK_FREE,      /// memory deallocation
    CK_FOPEN,     /// File open
    CK_FCLOSE     /// File close
  };

  using TDAPIMap = std::map<std::string, CHECKER_TYPE>;

private:
  /// API map, from a string to checker API type
  TDAPIMap tdAPIMap;

  /// Constructor
  SaberCheckerAPI() { init(); }

  /// Initialize the map
  void init();

  /// Static reference
  static SaberCheckerAPI *ckAPI;

  /// Get the function type of a function by name
  inline CHECKER_TYPE getType(const std::string &funName) const {
    auto it = tdAPIMap.find(funName);
    if (it != tdAPIMap.end())
      return it->second;
    return CK_DUMMY;
  }

public:
  /// Return a static reference
  static SaberCheckerAPI *getCheckerAPI() {
    if (ckAPI == nullptr) {
      ckAPI = new SaberCheckerAPI();
    }
    return ckAPI;
  }

  /// Return true if this call is a memory allocation
  inline bool isMemAlloc(const std::string &funName) const {
    return getType(funName) == CK_ALLOC;
  }

  /// Return true if this call is a memory deallocation
  inline bool isMemDealloc(const std::string &funName) const {
    return getType(funName) == CK_FREE;
  }

  /// Return true if this deallocation API frees through an extra load.
  ///
  /// SVF's SABER only applies the extra sink modeling for ExtAPI
  /// `EFT_FREE_MULTILEVEL` functions such as `XFree`, not for arbitrary
  /// pointer-to-pointer actual arguments.
  inline bool isMultiLevelMemDealloc(const std::string &funName) const {
    return funName == "XFree";
  }

  /// Return true if this call is a file open
  inline bool isFOpen(const std::string &funName) const {
    return getType(funName) == CK_FOPEN;
  }

  /// Return true if this call is a file close
  inline bool isFClose(const std::string &funName) const {
    return getType(funName) == CK_FCLOSE;
  }

  /// Overloads for LLVM call/function (faithful to SVF's
  /// FunObjVar/CallICFGNode)
  inline CHECKER_TYPE getType(llvm::Function const *F) const {
    if (F) {
      auto it = tdAPIMap.find(F->getName().str());
      if (it != tdAPIMap.end())
        return it->second;
    }
    return CK_DUMMY;
  }
  inline bool isMemAlloc(llvm::Function const *fun) const {
    return getType(fun) == CK_ALLOC;
  }
  inline bool isMemDealloc(llvm::Function const *fun) const {
    return getType(fun) == CK_FREE;
  }
  inline bool isMultiLevelMemDealloc(llvm::Function const *fun) const {
    return fun && isMultiLevelMemDealloc(fun->getName().str());
  }
  inline bool isFOpen(llvm::Function const *fun) const {
    return getType(fun) == CK_FOPEN;
  }
  inline bool isFClose(llvm::Function const *fun) const {
    return getType(fun) == CK_FCLOSE;
  }
  inline bool isMemAlloc(llvm::CallBase const *cs) const {
    llvm::Function const *F = cs ? cs->getCalledFunction() : nullptr;
    return isMemAlloc(F);
  }
  inline bool isMemDealloc(llvm::CallBase const *cs) const {
    llvm::Function const *F = cs ? cs->getCalledFunction() : nullptr;
    return isMemDealloc(F);
  }
  inline bool isMultiLevelMemDealloc(llvm::CallBase const *cs) const {
    llvm::Function const *F = cs ? cs->getCalledFunction() : nullptr;
    return isMultiLevelMemDealloc(F);
  }
  inline bool isFOpen(llvm::CallBase const *cs) const {
    llvm::Function const *F = cs ? cs->getCalledFunction() : nullptr;
    return isFOpen(F);
  }
  inline bool isFClose(llvm::CallBase const *cs) const {
    llvm::Function const *F = cs ? cs->getCalledFunction() : nullptr;
    return isFClose(F);
  }
  bool isExtCall(llvm::Function const *fun) const;
};

} // namespace analysis
} // namespace lotus

#endif /* SABERCHECKERAPI_H_ */
