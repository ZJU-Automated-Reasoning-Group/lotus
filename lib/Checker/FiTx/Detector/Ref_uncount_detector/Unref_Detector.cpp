#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include "Checker/FiTx/Detector/Unref_Detector.h"

namespace {
class UnrefCountDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    UnreferenceCounter::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new UnrefCountDetector()};
