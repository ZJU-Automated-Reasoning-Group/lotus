//===- FileChecker.h -- Checking incorrect file-open close errors-------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#ifndef FILECHECK_H_
#define FILECHECK_H_

#include "Checker/Saber/LeakChecker.h"

#include <string>

namespace lotus {
namespace analysis {

class FileChecker : public LeakChecker {

public:
  FileChecker() = default;
  virtual ~FileChecker() = default;

  bool runOnModule(llvm::Module &M) {
    setModule(&M);
    analyze();
    return false;
  }

  inline bool isSourceLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isFOpen(funName);
  }

  inline bool isSinkLikeFun(const std::string &funName) override {
    return SaberCheckerAPI::getCheckerAPI()->isFClose(funName);
  }

  void reportBug(ProgSlice *slice) override;
};

} // namespace analysis
} // namespace lotus

#endif
