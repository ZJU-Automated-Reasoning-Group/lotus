// Double Free (DF) detector: typestate FSM for double-free bug pattern.
// Paper: Suzuki et al., USENIX ATC 2024, Section 4.1 (typestate for DF),
// Figure 3 (init→free→DF), Table 5 (Calls kfree, Store anything).
#include "Checker/FiTx/Detector/DF_Detector.h"

#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class DoubleFreeDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    DoubleFree::define_states(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new DoubleFreeDetector()};
