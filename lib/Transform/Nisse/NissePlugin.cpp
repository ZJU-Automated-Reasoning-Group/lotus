//===-- NissePlugin.cpp ------------------------------------------------===//
// Copyright (C) 2023 Leon Frenot
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//===----------------------------------------------------------------------===//
///
/// \file
/// This file is the entry point for the New PM opt plugin. That is, it
/// contains the New PM registration for all the analyses and transformations
/// related to the Nisse plugin.
//===----------------------------------------------------------------------===//
//
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Transforms/Utils/BreakCriticalEdges.h"

#include "Transform/Nisse/Nisse.h"

using namespace llvm;

/// Takes a FunctionAnalysisManager \p FAM and uses it to register all the
/// analyses created, so any pass can request their results.
void registerAnalyses(FunctionAnalysisManager &FAM) {
  FAM.registerPass([] { return nisse::NisseAnalysis(); });
  FAM.registerPass([] { return nisse::KSAnalysis(); });
}

/// Takes the \p Name of a transformation pass and check if it is the name of
/// any of the passes implemented. If so, add it to the FunctionPassManager \p
/// FPM.
///
/// \returns true if \p Name corresponds to any of the passes implemented;
/// otherwise, returns false.
bool registerPipeline(StringRef Name, ModulePassManager &MPM,
                      ArrayRef<PassBuilder::PipelineElement>) {

  if (Name == "nisse") {
    // FunctionPassManager FPM;
    // FPM.addPass(LoopSimplifyPass());
    // FPM.addPass(BreakCriticalEdgesPass());

    // MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.addPass(nisse::NissePass());
    return true;
  }

  if (Name == "ks") {
    FunctionPassManager FPM;
    FPM.addPass(LoopSimplifyPass());
    FPM.addPass(BreakCriticalEdgesPass());

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
    MPM.addPass(nisse::KSPass());
    return true;
  }

  return false;
}
/// \brief Registers the plugins for the pipelines
/// \return The plugin info for both transformation passes
PassPluginLibraryInfo getNissePluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "Nisse", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            // 1: Register the NisseAnalysis as an analysis pass so that
            // it can be requested by other passes as following:
            // FPM.getResult<NisseAnalysis>(F), where FPM is the
            // FunctionAnalysisManager and F is the Function that shall be
            // analyzed.
            PB.registerAnalysisRegistrationCallback(registerAnalyses);

            // 2: Register the NissePrinterPass as "print<add-const>" so
            // that it can be used when specifying pass pipelines with
            // "-passes=". Also register NissePass as "add-const".
            PB.registerPipelineParsingCallback(registerPipeline);
          }};
}

/// \brief The public entry point for a pass plugin:
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getNissePluginInfo();
}
