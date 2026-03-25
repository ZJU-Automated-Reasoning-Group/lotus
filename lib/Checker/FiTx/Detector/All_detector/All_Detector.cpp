#include "Checker/FiTx/Detector/All_Detector.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class AllDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    for (auto& define_states : def_funcs) {
      fitx::StateManager manager;
      define_states(manager);
      addStateManager(manager);
    }
  }
};
}  // namespace

std::vector<fitx::FrameworkPass*> fitx::FrameworkPass::passes = {
    new AllDetector()};
