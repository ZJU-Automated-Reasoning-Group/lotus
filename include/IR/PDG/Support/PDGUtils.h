/**
 * @file PDGUtils.h
 * @brief Header for PDG utility functions
 */

#pragma once

#include "IR/PDG/Core/Tree.h"
#include "IR/PDG/Support/LLVMEssentials.h"

#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pdg {
class TreeNode;

namespace pdgutils {
// Struct and GEP handling
llvm::StructType *getStructTypeFromGEP(llvm::GetElementPtrInst &gep);
int getGEPAccessFieldOffset(llvm::GetElementPtrInst &gep);
int64_t getGEPOffsetInBits(llvm::Module &M, llvm::StructType &struct_type,
                           llvm::GetElementPtrInst &gep);
bool isNodeBitOffsetMatchGEPBitOffset(Node &n, llvm::GetElementPtrInst &gep);
bool isGEPOffsetMatchDIOffset(llvm::DIType &dt, llvm::GetElementPtrInst &gep);

// Call handling
llvm::Function *getCalledFunc(llvm::CallBase &call_inst);

// Access analysis
bool hasReadAccess(llvm::Value &v);
bool hasWriteAccess(llvm::Value &v);

// Global variable analysis
bool isStaticFuncVar(llvm::GlobalVariable &gv, llvm::Module &M);
bool isStaticGlobalVar(llvm::GlobalVariable &gv);

// Instruction traversal
//
// These helpers perform full function scans and should be treated as
// compatibility/non-hot-path utilities. Prefer local scans or O(1) lookup
// structures in performance-sensitive call sites.
llvm::inst_iterator getInstIter(llvm::Instruction &i);
std::set<llvm::Instruction *> getInstructionBeforeInst(llvm::Instruction &i);
std::set<llvm::Instruction *> getInstructionAfterInst(llvm::Instruction &i);

// Allocation analysis
std::set<llvm::Value *> computeAddrTakenVarsFromAlloc(llvm::AllocaInst &ai);

// Printing and string formatting
void printTreeNodesLabel(Node *n, llvm::raw_string_ostream &OS,
                         std::string tree_node_type_str);
llvm::Value *getLShrOnGep(llvm::GetElementPtrInst &gep);
std::string stripFuncNameVersionNumber(std::string func_name);
std::string computeTreeNodeID(TreeNode &tree_node);
std::string stripVersionTag(std::string str);
std::string getNodeTypeStr(GraphNodeType node_type);
std::string getEdgeTypeStr(EdgeType edge_type);
std::string &rtrim(std::string &s, const char *t = "\t\n\r\f\v");
} // namespace pdgutils
} // namespace pdg
