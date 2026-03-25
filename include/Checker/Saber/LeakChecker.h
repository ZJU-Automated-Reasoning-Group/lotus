//===- LeakChecker.h -- Detecting memory leaks--------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef LEAKCHECKER_H_
#define LEAKCHECKER_H_

#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SrcSnkDDA.h"

#include <string>

namespace lotus {
namespace analysis {

class LeakChecker : public SrcSnkDDA {

public:
  enum LEAK_TYPE { NEVER_FREE_LEAK, CONTEXT_LEAK, PATH_LEAK, GLOBAL_LEAK };

  LeakChecker() = default;
  virtual ~LeakChecker() = default;

  bool runOnModule(llvm::Module &M) {
    setModule(&M);
    analyze();
    return false;
  }

  void initSrcs() override;
  void initSnks() override;

  inline bool isSourceLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isMemAlloc(funName);
  }

  inline bool isSinkLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isMemDealloc(funName);
  }

protected:
  void reportBug(ProgSlice *slice) override;

  void testsValidation(const ProgSlice *slice);
  void validateSuccessTests(const SVFGNode *source, const std::string &fun);
  void validateExpectedFailureTests(const SVFGNode *source,
                                    const std::string &fun);
};

} // namespace analysis
} // namespace lotus

#endif
