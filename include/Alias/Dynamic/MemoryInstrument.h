#pragma once

namespace llvm {
class Module;
} // namespace llvm

namespace dynamic {

class MemoryInstrument {
private:
public:
  MemoryInstrument() = default;

  void runOnModule(llvm::Module &);
};
} // namespace dynamic
