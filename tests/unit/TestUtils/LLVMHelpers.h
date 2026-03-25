#ifndef LOTUS_UNITTEST_TESTUTILS_LLVMHELPERS_H_
#define LOTUS_UNITTEST_TESTUTILS_LLVMHELPERS_H_

#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace llvm;

namespace lotus {
namespace unittest {

inline std::unique_ptr<Module> parseModule(LLVMContext &context,
                                           const char *source,
                                           StringRef diag_owner =
                                               "LLVMHelpers") {
  SMDiagnostic err;
  auto module = parseAssemblyString(source, err, context);
  if (!module) {
    err.print(diag_owner.data(), errs());
  }
  return module;
}

inline std::unique_ptr<Module> parseModule(LLVMContext &context,
                                           const std::string &source,
                                           StringRef diag_owner =
                                               "LLVMHelpers") {
  return parseModule(context, source.c_str(), diag_owner);
}

inline std::unique_ptr<Module> parseModuleChecked(LLVMContext &context,
                                                  const char *source,
                                                  StringRef diag_owner =
                                                      "LLVMHelpers") {
  auto module = parseModule(context, source, diag_owner);
  EXPECT_NE(module, nullptr);
  return module;
}

inline std::unique_ptr<Module> parseModuleChecked(LLVMContext &context,
                                                  const std::string &source,
                                                  StringRef diag_owner =
                                                      "LLVMHelpers") {
  return parseModuleChecked(context, source.c_str(), diag_owner);
}

inline std::unique_ptr<Module> parseAssembly(LLVMContext &context,
                                             const char *source,
                                             StringRef diag_owner =
                                                 "LLVMHelpers") {
  return parseModule(context, source, diag_owner);
}

inline std::unique_ptr<Module> parseAssembly(LLVMContext &context,
                                             const std::string &source,
                                             StringRef diag_owner =
                                                 "LLVMHelpers") {
  return parseModule(context, source, diag_owner);
}

inline std::unique_ptr<Module> parseAssemblyChecked(LLVMContext &context,
                                                    const char *source,
                                                    StringRef diag_owner =
                                                        "LLVMHelpers") {
  return parseModuleChecked(context, source, diag_owner);
}

inline std::unique_ptr<Module> parseAssemblyChecked(LLVMContext &context,
                                                    const std::string &source,
                                                    StringRef diag_owner =
                                                        "LLVMHelpers") {
  return parseModuleChecked(context, source, diag_owner);
}

inline std::unique_ptr<Module> loadModule(StringRef path,
                                          LLVMContext &context,
                                          StringRef diag_owner =
                                              "LLVMHelpers") {
  SMDiagnostic err;
  auto module = parseIRFile(path, err, context);
  if (!module) {
    err.print(diag_owner.data(), errs());
  }
  return module;
}

inline std::unique_ptr<Module> loadModule(const std::string &path,
                                          LLVMContext &context,
                                          StringRef diag_owner =
                                              "LLVMHelpers") {
  return loadModule(StringRef(path), context, diag_owner);
}

inline Instruction *findInstructionByName(Function &func, StringRef name) {
  for (auto &bb : func) {
    for (auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

inline Instruction *findInstructionByName(Function *func, StringRef name) {
  return func ? findInstructionByName(*func, name) : nullptr;
}

inline const Instruction *findInstructionByName(const Function &func,
                                                StringRef name) {
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

inline const Instruction *findInstructionByName(const Function *func,
                                                StringRef name) {
  return func ? findInstructionByName(*func, name) : nullptr;
}

inline Instruction *findInst(Function &func, StringRef name) {
  return findInstructionByName(func, name);
}

inline Instruction *findInst(Function *func, StringRef name) {
  return findInstructionByName(func, name);
}

inline std::vector<CallBase *> findCallsTo(Function &func,
                                           StringRef callee_name) {
  std::vector<CallBase *> calls;
  for (auto &bb : func) {
    for (auto &inst : bb) {
      auto *call = dyn_cast<CallBase>(&inst);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == callee_name) {
        calls.push_back(call);
      }
    }
  }
  return calls;
}

inline std::vector<CallBase *> findCallsTo(Function *func,
                                           StringRef callee_name) {
  return func ? findCallsTo(*func, callee_name) : std::vector<CallBase *>{};
}

inline std::vector<const CallBase *> findCallsTo(const Function &func,
                                                 StringRef callee_name) {
  std::vector<const CallBase *> calls;
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      auto *call = dyn_cast<CallBase>(&inst);
      if (!call || !call->getCalledFunction()) {
        continue;
      }
      if (call->getCalledFunction()->getName() == callee_name) {
        calls.push_back(call);
      }
    }
  }
  return calls;
}

inline std::vector<const CallBase *> findCallsTo(const Function *func,
                                                 StringRef callee_name) {
  return func ? findCallsTo(*func, callee_name)
              : std::vector<const CallBase *>{};
}

inline CallBase *findCallTo(Function &func, StringRef callee_name) {
  auto calls = findCallsTo(func, callee_name);
  return calls.empty() ? nullptr : calls.front();
}

inline CallBase *findCallTo(Function *func, StringRef callee_name) {
  return func ? findCallTo(*func, callee_name) : nullptr;
}

inline const CallBase *findCallTo(const Function &func, StringRef callee_name) {
  auto calls = findCallsTo(func, callee_name);
  return calls.empty() ? nullptr : calls.front();
}

inline const CallBase *findCallTo(const Function *func, StringRef callee_name) {
  return func ? findCallTo(*func, callee_name) : nullptr;
}

inline std::vector<CallBase *> getIndirectCalls(Function &func) {
  std::vector<CallBase *> calls;
  for (Instruction &inst : instructions(func)) {
    auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->isIndirectCall()) {
      calls.push_back(call);
    }
  }
  return calls;
}

inline std::vector<const CallBase *> getIndirectCalls(const Function &func) {
  std::vector<const CallBase *> calls;
  for (const Instruction &inst : instructions(func)) {
    auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->isIndirectCall()) {
      calls.push_back(call);
    }
  }
  return calls;
}

inline CallBase *findIndirectCall(Function &func) {
  auto calls = getIndirectCalls(func);
  return calls.empty() ? nullptr : calls.front();
}

inline CallBase *findIndirectCall(Function *func) {
  return func ? findIndirectCall(*func) : nullptr;
}

inline const CallBase *findIndirectCall(const Function &func) {
  auto calls = getIndirectCalls(func);
  return calls.empty() ? nullptr : calls.front();
}

inline const CallBase *findIndirectCall(const Function *func) {
  return func ? findIndirectCall(*func) : nullptr;
}

inline const BasicBlock *findBasicBlockByName(const Function &func,
                                              StringRef name) {
  for (const auto &bb : func) {
    if (bb.getName() == name) {
      return &bb;
    }
  }
  return nullptr;
}

inline BasicBlock *findBasicBlockByName(Function &func, StringRef name) {
  for (auto &bb : func) {
    if (bb.getName() == name) {
      return &bb;
    }
  }
  return nullptr;
}

inline BasicBlock *findBlock(Function &func, StringRef name) {
  return findBasicBlockByName(func, name);
}

inline BasicBlock *findBlock(Function *func, StringRef name) {
  return func ? findBasicBlockByName(*func, name) : nullptr;
}

inline const BasicBlock *findBlock(const Function &func, StringRef name) {
  return findBasicBlockByName(func, name);
}

inline const BasicBlock *findBlock(const Function *func, StringRef name) {
  return func ? findBasicBlockByName(*func, name) : nullptr;
}

inline PHINode *findPhi(Function &func, StringRef name) {
  for (auto &bb : func) {
    for (auto &phi : bb.phis()) {
      if (phi.getName() == name) {
        return &phi;
      }
    }
  }
  return nullptr;
}

inline PHINode *findPhi(Function *func, StringRef name) {
  return func ? findPhi(*func, name) : nullptr;
}

inline const PHINode *findPhi(const Function &func, StringRef name) {
  for (const auto &bb : func) {
    for (const auto &phi : bb.phis()) {
      if (phi.getName() == name) {
        return &phi;
      }
    }
  }
  return nullptr;
}

inline const PHINode *findPhi(const Function *func, StringRef name) {
  return func ? findPhi(*func, name) : nullptr;
}

inline const Instruction *getFirstInstruction(const Function &func) {
  if (func.empty()) {
    return nullptr;
  }
  return &func.getEntryBlock().front();
}

inline Function *findFunctionByName(Module &module, StringRef name) {
  return module.getFunction(name);
}

template <typename InstTy>
InstTy *findInstruction(Function &F, StringRef name = "") {
  for (auto &BB : F) {
    for (auto &I : BB) {
      auto *inst = dyn_cast<InstTy>(&I);
      if (inst && (name.empty() || I.getName() == name)) {
        return inst;
      }
    }
  }
  return nullptr;
}

template <typename InstTy>
const InstTy *findInstruction(const Function &F, StringRef name = "") {
  for (const auto &BB : F) {
    for (const auto &I : BB) {
      auto *inst = dyn_cast<InstTy>(&I);
      if (inst && (name.empty() || I.getName() == name)) {
        return inst;
      }
    }
  }
  return nullptr;
}

class LlvmModuleTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    return unittest::parseModule(context, source, "LlvmModuleTest");
  }

  std::unique_ptr<Module> parseModule(const std::string &source) {
    return unittest::parseModule(context, source, "LlvmModuleTest");
  }

  bool loadModule(const char *ir) {
    SMDiagnostic error;
    auto module = parseIR(MemoryBuffer::getMemBuffer(ir)->getMemBufferRef(),
                          error, context);
    if (!module) {
      error.print("LlvmModuleTest", errs());
      return false;
    }
    this->module = std::move(module);
    return true;
  }

  std::unique_ptr<Module> module;
};

} // namespace unittest
} // namespace lotus

#endif // LOTUS_UNITTEST_TESTUTILS_LLVMHELPERS_H_
