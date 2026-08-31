#include "Analysis/Purity/MemorySSAPuritySummary.h"

#include "IR/ShadowMemSSA/ShadowMemSSA.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"

namespace lotus::analysis::purity {

using namespace llvm;

namespace {

bool isShadowMemCall(const CallBase &call) {
  const Function *callee = call.getCalledFunction();
  return callee && callee->getName().startswith("shadow.mem");
}

bool moduleContainsShadowMem(const Module &module) {
  for (const Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (call && isShadowMemCall(*call)) {
        return true;
      }
    }
  }
  return false;
}

class DummyMemorySSAPass final : public ModulePass {
public:
  static char ID;

  DummyMemorySSAPass() : ModulePass(ID) {}

  bool runOnModule(Module &module) override {
    (void)module;
    return false;
  }
};

char DummyMemorySSAPass::ID = 0;

} // namespace

class MemorySSAPuritySummaryProvider::Impl {
public:
  explicit Impl(Module &module)
      : hasInstrumentedIR_(moduleContainsShadowMem(module)) {
    if (hasInstrumentedIR_) {
      manager_ = std::make_unique<previrt::analysis::ShadowMemSSACallsManager>(
          module, pass_, false);
    }
  }

  bool hasInstrumentedIR_ = false;
  DummyMemorySSAPass pass_;
  std::unique_ptr<previrt::analysis::ShadowMemSSACallsManager> manager_;
};

MemorySSAPuritySummaryProvider::MemorySSAPuritySummaryProvider(Module &module)
    : impl_(std::make_unique<Impl>(module)) {}

MemorySSAPuritySummaryProvider::~MemorySSAPuritySummaryProvider() = default;

bool MemorySSAPuritySummaryProvider::hasInstrumentedIR() const {
  return impl_->hasInstrumentedIR_;
}

std::optional<MemorySSAPuritySummary>
MemorySSAPuritySummaryProvider::getFunctionSummary(
    const Function &function) const {
  if (!hasInstrumentedIR() || function.isDeclaration()) {
    return std::nullopt;
  }

  const auto *mssaFunction = impl_->manager_->getFunction(&function);
  if (!mssaFunction) {
    return std::nullopt;
  }

  MemorySSAPuritySummary summary;
  summary.readsReachableMemory = mssaFunction->getNumInFormals() > 0;
  summary.writesReachableMemory = mssaFunction->getNumOutFormals() > 0;
  return summary;
}

std::optional<MemorySSAPuritySummary>
MemorySSAPuritySummaryProvider::getCallSummary(const CallBase &call) const {
  if (!hasInstrumentedIR()) {
    return std::nullopt;
  }

  const auto *callSite = impl_->manager_->getCallSite(&call);
  if (!callSite) {
    return std::nullopt;
  }

  MemorySSAPuritySummary summary;
  for (unsigned idx = 0; idx < callSite->numParams(); ++idx) {
    if (callSite->isRef(idx) || callSite->isRefMod(idx)) {
      summary.readsReachableMemory = true;
    }
    if (callSite->isMod(idx) || callSite->isRefMod(idx) ||
        callSite->isNew(idx)) {
      summary.writesReachableMemory = true;
    }
  }
  return summary;
}

} // namespace lotus::analysis::purity
