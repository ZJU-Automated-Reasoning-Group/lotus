//===- DDAStat.cpp -- DDA statistics (SVF-style) -------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DDAStat.h"

#include "Alias/DDA/CxtDPItem.h"
#include "Alias/DDA/FlowDDA.h"

#include <llvm/Support/raw_ostream.h>

using namespace lotus::analysis;

DDAStat::DDAStat(FlowDDA *pta) : pta_(pta) {}

void DDAStat::performStat() {
  maximumCxtSeen = ContextCond::getMaxCxtLenSeen();
}

void DDAStat::printStat(const std::string &str) {
  llvm::outs() << "=== DDA Stat " << str << " ===\n";
  llvm::outs() << "  NumOfStep: " << numOfStep << "\n";
  llvm::outs() << "  NumOfDPM: " << numOfDPM << "\n";
  llvm::outs() << "  NumQueries: " << numQueries << "\n";
  llvm::outs() << "  NumOutOfBudgetQueries: " << numOutOfBudgetQueries << "\n";
  llvm::outs() << "  NumOfStrongUpdates: " << numOfStrongUpdates << "\n";
  llvm::outs() << "  NumOfMustAliases: " << numOfMustAliases << "\n";
  llvm::outs() << "  NumOfInfeasiblePath: " << numOfInfeasiblePath << "\n";
  llvm::outs() << "  NumOfStepInCycle: " << numOfStepInCycle << "\n";
  llvm::outs() << "  TotalTimeOfQueries: " << totalTimeOfQueries << "\n";
  llvm::outs() << "  MaximumCxtSeen: " << maximumCxtSeen << "\n";
  llvm::outs() << "  MaximumPathSeen: " << maximumPathSeen << "\n";
  llvm::outs() << "========================\n";
}
