#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class RefCountDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    ReferenceCounter::defineStates(manager);
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new RefCountDetector()};
