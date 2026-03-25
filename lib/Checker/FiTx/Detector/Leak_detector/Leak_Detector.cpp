#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

namespace {
class LeakDetector : public fitx::FrameworkPass {
  virtual void defineStates() override {
    fitx::StateManager manager;
    MemoryLeak::defineStates(manager);  
    addStateManager(manager);
  }
};

}  // namespace

std::vector<fitx::FrameworkPass *> fitx::FrameworkPass::passes = {
    new LeakDetector()};
