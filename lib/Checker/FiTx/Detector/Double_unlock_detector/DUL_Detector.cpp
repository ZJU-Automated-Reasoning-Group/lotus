#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Detector/DUL_Detector.h"

namespace {
class DoubleUnlockDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    DoubleUnlock::defineStates(manager);
    addStateManager(manager);
  }
};
}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new DoubleUnlockDetector()};
