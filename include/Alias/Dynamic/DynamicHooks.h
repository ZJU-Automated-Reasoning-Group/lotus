#pragma once

namespace llvm {
class Function;
class Module;
class Type;
} // namespace llvm

namespace dynamic {

class DynamicHooks {
private:
  llvm::Function *initHook;
  llvm::Function *allocHook;
  llvm::Function *pointerHook;
  llvm::Function *callHook;
  llvm::Function *enterHook;
  llvm::Function *exitHook;
  llvm::Function *globalHook;
  llvm::Function *mainHook;

public:
  DynamicHooks(llvm::Module &);

  llvm::Function *getInitHook() { return initHook; }
  llvm::Function *getAllocHook() { return allocHook; }
  llvm::Function *getPointerHook() { return pointerHook; }
  llvm::Function *getCallHook() { return callHook; }
  llvm::Function *getEnterHook() { return enterHook; }
  llvm::Function *getExitHook() { return exitHook; }
  llvm::Function *getGlobalHook() { return globalHook; }
  llvm::Function *getMainHook() { return mainHook; }

  bool isHook(const llvm::Function &) const;
};
} // namespace dynamic
