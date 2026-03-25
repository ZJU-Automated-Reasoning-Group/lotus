#pragma once

namespace llvm {
class Function;
class Module;
class StringRef;
class Value;
} // namespace llvm

namespace dynamic {

class FeatureCheck {
private:
  void issueWarning(const llvm::Value &, const llvm::StringRef &);

  void checkIndirectLibraryCall(const llvm::Function &f);
  void checkArrayArgOrInst(const llvm::Function &f);

public:
  FeatureCheck() = default;

  void runOnModule(const llvm::Module &module);
};

} // namespace dynamic
