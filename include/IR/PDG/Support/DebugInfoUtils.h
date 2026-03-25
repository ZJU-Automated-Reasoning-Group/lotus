/**
 * @file DebugInfoUtils.h
 * @brief Header for debug information utilities
 */

#pragma once

#include "IR/PDG/Support/LLVMEssentials.h"

#include <queue>
#include <set>
#include <string>

namespace pdg {
namespace dbgutils {
// Type checking predicates
bool isPointerType(llvm::DIType &dt);
bool isReferenceType(llvm::DIType &dt);
bool isStructType(llvm::DIType &dt);
bool isClassType(llvm::DIType &dt);
bool isClassPointerType(llvm::DIType &dt);
bool isUnionType(llvm::DIType &dt);
bool isStructPointerType(llvm::DIType &dt);
bool isFuncPointerType(llvm::DIType &dt);
bool isProjectableType(llvm::DIType &dt);

bool hasSameDIName(llvm::DIType &d1, llvm::DIType &d2);

// Type navigation and extraction
llvm::DIType *getLowestDIType(llvm::DIType &dt);
llvm::DIType *getBaseDIType(llvm::DIType &dt);
llvm::DIType *stripAttributes(llvm::DIType &dt);
llvm::DIType *stripMemberTag(llvm::DIType &dt);

// Debug info retrieval for LLVM values
llvm::DIType *getGlobalVarDIType(llvm::GlobalVariable &gv);
llvm::DIType *getFuncRetDIType(llvm::Function &F);

// Source level name extraction
std::string getSourceLevelVariableName(llvm::DINode &dt);
std::string getSourceLevelTypeName(llvm::DIType &dt);

// Structural analysis
std::set<llvm::DIType *> computeContainedStructTypes(llvm::DIType &dt);
} // namespace dbgutils
} // namespace pdg
