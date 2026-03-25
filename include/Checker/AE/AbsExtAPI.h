//===- AbsExtAPI.h -- Abstract Interpretation External API handler--//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractState.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>

namespace lotus {
namespace analysis {

class AbstractInterpretation;

class AEExtAPI {
public:
  enum ExtAPIType {
    UNCLASSIFIED,
    MEMCPY,
    MEMSET,
    STRCPY,
    STRCAT,
    ALLOC,
    REALLOC,
    FREE,
    STRLEN,
    SCANF,
    PRINTF,
    RECV,
    ITOA,
    SNPRINTF,
    // Additional types from SVF
    ALLOC_ARG0, // Allocates and stores result in argument 0
    STRTOK,     // String tokenization
    STRCHR,     // String character search
    STRSTR,     // String substring search
    STRPBRK,    // String character set search
    FGETS,      // File string input
    FREAD,      // File read
    FWRITE,     // File write
    TIME,       // Time functions
    ENV,        // Environment functions
    STRTO       // String to number conversion
  };

  AEExtAPI(std::map<const llvm::Instruction *, AbstractState> &traces);

  void initExtFunMap();

  std::string strRead(AbstractState &as, uint32_t strId);

  void handleExtAPI(const llvm::CallBase *call,
                    const llvm::Function *callee = nullptr);

  void handleStrcpy(const llvm::CallBase *call);
  void handleStrcat(const llvm::CallBase *call);

  void handleExtMemcpy(const llvm::CallBase *call);
  void handleExtMemset(const llvm::CallBase *call);
  void handleExtStrcpy(const llvm::CallBase *call);
  void handleExtStrcat(const llvm::CallBase *call);
  void handleExtAlloc(const llvm::CallBase *call);
  void handleExtRealloc(const llvm::CallBase *call);
  void handleExtFree(const llvm::CallBase *call);
  void handleExtStrlen(const llvm::CallBase *call);
  void handleExtScanf(const llvm::CallBase *call);
  void handleExtSnprintf(const llvm::CallBase *call);
  void handleExtRecv(const llvm::CallBase *call);

  // Additional handlers for new types
  void handleExtAllocArg0(const llvm::CallBase *call);
  void handleExtStrtok(const llvm::CallBase *call);
  void handleExtStrchr(const llvm::CallBase *call);
  void handleExtStrstr(const llvm::CallBase *call);
  void handleExtFgets(const llvm::CallBase *call);
  void handleExtFread(const llvm::CallBase *call);
  void handleExtTime(const llvm::CallBase *call);
  void handleExtEnv(const llvm::CallBase *call);
  void handleExtStrto(const llvm::CallBase *call);

  IntervalValue getStrlen(AbstractState &as, uint32_t strId);

  void handleMemcpy(AbstractState &as, uint32_t dstId, uint32_t srcId,
                    IntervalValue len, uint32_t start_idx);

  void handleMemset(AbstractState &as, uint32_t dstId, IntervalValue elem,
                    IntervalValue len);

  IntervalValue getRangeLimitFromType(llvm::Type *type);

  AbstractState &getAbsStateFromTrace(const llvm::Instruction *val);

  static uint32_t getValueId(const llvm::Value *val);

  std::vector<std::string> getExtFuncAnnotations(const llvm::Function *fun);

  bool hasExtFuncAnnotation(const llvm::Function *fun,
                            const std::string &annotation);

  ExtAPIType getExtAPIType(const llvm::Function *fun);

protected:
  void initAnnotationMap();
  void addAnnotation(const std::string &funcName,
                     const std::vector<std::string> &annotations);

  llvm::Module *module_;
  std::map<const llvm::Instruction *, AbstractState> &abstractTrace;
  std::map<std::string, std::function<void(const llvm::CallBase *)>> func_map;

  std::map<std::string, std::vector<std::string>> funcAnnotations;
};

} // namespace analysis
} // namespace lotus
