#pragma once

#include "Checker/FiTx/Detector/Lock.h"
#include "Checker/FiTx/Frontend/State.h"

namespace DoubleLock {
void define_states(fitx::StateManager &manager);
} // namespace DoubleLock
