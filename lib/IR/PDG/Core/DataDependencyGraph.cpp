/**
 * @file DataDependencyGraph.cpp
 * @brief Implementation of the data dependency analysis for the PDG
 *
 * This file implements the DataDependencyGraph pass, which analyzes data
 * dependencies between program elements. Data dependencies occur when one
 * instruction defines a value that is used by another instruction (def-use
 * chains).
 *
 * Key features:
 * - Analysis of def-use chains in LLVM IR
 * - Support for different types of data dependencies (direct, memory, etc.)
 * - Function-level data dependency analysis
 * - Integration with the overall PDG framework
 * - Support for memory-based dependencies through load/store analysis
 *
 * The data dependency analysis is a fundamental component of the PDG system,
 * complementing control dependency analysis to provide a complete view of
 * program dependencies.
 */

#include "IR/PDG/Core/DataDependencyGraph.h"

#include "IR/PDG/Core/PDGAliasWrapper.h"
#include "IR/PDG/Support/PDGCommandLineOptions.h"

#include <algorithm>
#include <cctype>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using llvm::cl::desc;
using llvm::cl::init;
using llvm::cl::opt;

namespace {
// Fast filter: only consider instructions that touch or produce pointers.
static bool isAliasRelevantInst(const llvm::Instruction &I) {
  if (I.mayReadOrWriteMemory() || I.getType()->isPointerTy())
    return true;
  for (const auto &Op : I.operands())
    if (Op->getType()->isPointerTy())
      return true;
  return false;
}

// Command-line knobs to choose alias analyses for data dependence construction.
// -pdg-aa : over-approximate (sound) AA used to add alias edges (default:
// Andersen). -pdg-aa-under : under-approximate AA used to confirm must-alias
// edges (default: UnderApprox, use "none" to disable).
static opt<std::string> PdgAliasOverOpt(
    "pdg-aa",
    desc("Alias analysis used for PDG data deps "
         "(andersen, andersen-1cfa, andersen-2cfa, dyck, cfl-anders, "
         "cfl-steens, combined, underapprox)"),
    init("andersen"));

static opt<std::string> PdgAliasUnderOpt(
    "pdg-aa-under",
    desc("Under-approximate alias analysis for must-alias pruning "
         "(underapprox|none)"),
    init("underapprox"));

// Map a user-facing string to an AAConfig. Defaults to the provided fallback
// when the string is unknown.
static pdg::AAConfig parseAAConfig(const std::string &aa,
                                   const pdg::AAConfig &fallback) {
  return lotus::parseAAConfigFromString(aa, fallback);
}

// Helper that builds an alias wrapper or returns nullptr when disabled/failed.
static std::unique_ptr<pdg::PDGAliasWrapper>
buildAliasWrapper(llvm::Module &M, const std::string &userChoice,
                  const pdg::AAConfig &fallback, const char *label) {
  std::string lower = userChoice;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  if (lower == "none" || lower == "off" || lower == "disable") {
    llvm::errs() << "pdg: " << label << " alias analysis disabled by flag\n";
    return nullptr;
  }

  auto config = parseAAConfig(userChoice, fallback);
  auto wrapper = pdg::PDGAliasFactory::create(M, config);

  if (!wrapper || !wrapper->isInitialized()) {
    llvm::errs() << "pdg: failed to initialize " << label << " alias analysis: "
                 << pdg::PDGAliasFactory::getTypeName(config) << "\n";
    return nullptr;
  }

  if (pdg::DEBUG)
    llvm::errs() << "pdg: using " << pdg::PDGAliasFactory::getTypeName(config)
                 << " for " << label << " alias queries\n";

  return wrapper;
}
} // namespace

char pdg::DataDependencyGraph::ID = 0;

using namespace llvm;

bool pdg::DataDependencyGraph::runOnModule(Module &M) {
  ProgramGraph &g = ProgramGraph::getInstance();
  if (!g.isBuiltForModule(M)) {
    g.reset();
    g.build(M);
    g.bindDITypeToNodes(M);
  }

  // Initialize alias analysis wrappers based on command-line choices.
  _alias_wrapper_over =
      buildAliasWrapper(M, PdgAliasOverOpt.getValue(),
                        pdg::AAConfig::SparrowAA_NoCtx(), "over-approximate");
  _alias_wrapper_under =
      buildAliasWrapper(M, PdgAliasUnderOpt.getValue(),
                        pdg::AAConfig::UnderApprox(), "under-approximate");

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    _mem_dep_res = &getAnalysis<MemoryDependenceWrapperPass>(F).getMemDep();
    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      addDefUseEdges(*inst_iter);
      addRAWEdges(*inst_iter);
    }
    // Build alias edges once per function (not once per instruction) to avoid
    // O(n³) complexity.  Also guarded against AA-unavailable blowup.
    addAliasEdgesForFunction(F);
  }
  return false;
}

void pdg::DataDependencyGraph::addAliasEdges(Instruction &inst) {
  // This method is intentionally left as a no-op here.
  // Alias edges are now built once per function in addAliasEdgesForFunction()
  // to avoid the O(n²) per-instruction outer loop that the original design
  // produced (addAliasEdges was called for every instruction, making the
  // overall complexity O(n³) for a function with n instructions).
  (void)inst;
}

void pdg::DataDependencyGraph::addAliasEdgesForFunction(Function &F) {
  // When the over-approximate AA failed to initialize it returns MayAlias for
  // every pair, which would add a DATA_ALIAS edge between every two
  // pointer-touching instructions — an O(n²) graph blowup that makes all
  // alias-based queries useless.  Guard against this by skipping alias-edge
  // construction entirely when the over-approximate wrapper is unavailable.
  if (!_alias_wrapper_over || !_alias_wrapper_over->isInitialized()) {
    if (pdg::DEBUG)
      llvm::errs() << "pdg: skipping alias edges for " << F.getName()
                   << " (over-approximate AA not available)\n";
    return;
  }

  ProgramGraph &g = ProgramGraph::getInstance();

  // Collect alias-relevant instructions once.
  llvm::SmallVector<Instruction *, 64> relevant;
  for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F); ++inst_iter) {
    if (isAliasRelevantInst(*inst_iter))
      relevant.push_back(&*inst_iter);
  }

  // O(n²) over relevant instructions only (not all instructions).
  for (unsigned i = 0; i < relevant.size(); ++i) {
    Instruction *a = relevant[i];
    Node *src = g.getNode(*a);
    if (!src)
      continue;

    for (unsigned j = i + 1; j < relevant.size(); ++j) {
      Instruction *b = relevant[j];

      // Fast path: if both AAs agree on NoAlias, skip.
      auto over_result = queryAliasOverApproximate(*a, *b);
      if (over_result == llvm::AliasResult::NoAlias)
        continue;

      // Under-approximate AA can confirm NoAlias even when over says MayAlias.
      auto under_result = queryAliasUnderApproximate(*a, *b);
      if (under_result == llvm::AliasResult::NoAlias)
        continue;

      Node *dst = g.getNode(*b);
      if (!dst)
        continue;

      // Add bidirectional alias edges (alias is symmetric).
      src->addNeighbor(*dst, EdgeType::DATA_ALIAS);
      dst->addNeighbor(*src, EdgeType::DATA_ALIAS);
    }
  }
}

void pdg::DataDependencyGraph::addDefUseEdges(Instruction &inst) {
  ProgramGraph &g = ProgramGraph::getInstance();
  for (auto *user : inst.users()) {
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*user);
    if (src == nullptr || dst == nullptr)
      continue;
    EdgeType edge_type = EdgeType::DATA_DEF_USE;
    if (dst->getNodeType() == GraphNodeType::ANNO_VAR)
      edge_type = EdgeType::ANNO_VAR;
    if (dst->getNodeType() == GraphNodeType::ANNO_GLOBAL)
      edge_type = EdgeType::ANNO_GLOBAL;
    src->addNeighbor(*dst, edge_type);
  }
}

void pdg::DataDependencyGraph::addRAWEdges(Instruction &inst) {
  if (!isa<LoadInst>(&inst))
    return;

  ProgramGraph &g = ProgramGraph::getInstance();
  auto dep_res = _mem_dep_res->getDependency(&inst);
  auto *dep_inst = dep_res.getInst();

  if (dep_inst && dep_inst != &inst && dep_inst->mayWriteToMemory()) {
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*dep_inst);
    if (src != nullptr && dst != nullptr)
      dst->addNeighbor(*src, EdgeType::DATA_RAW);
  }

  // Non-local dependencies: walk defs/clobbers in other blocks.
  llvm::SmallVector<llvm::NonLocalDepResult, 8> non_local_deps;
  _mem_dep_res->getNonLocalPointerDependency(&inst, non_local_deps);
  for (auto &dep : non_local_deps) {
    auto res = dep.getResult();
    if (!res.isDef() && !res.isClobber())
      continue;
    Instruction *nl_inst = res.getInst();
    if (!nl_inst || nl_inst == &inst || !nl_inst->mayWriteToMemory())
      continue;
    Node *src = g.getNode(inst);
    Node *dst = g.getNode(*nl_inst);
    if (src != nullptr && dst != nullptr)
      dst->addNeighbor(*src, EdgeType::DATA_RAW);
  }
}

llvm::AliasResult
pdg::DataDependencyGraph::queryAliasUnderApproximate(llvm::Value &v1,
                                                     llvm::Value &v2) {
  // Use the under-approximation wrapper (syntactic pattern matching)
  // This only returns MustAlias for clear syntactic patterns, otherwise NoAlias
  if (_alias_wrapper_under && _alias_wrapper_under->isInitialized()) {
    return _alias_wrapper_under->query(&v1, &v2);
  }

  // Fall back to simple check if wrapper is not available
  if (!v1.getType()->isPointerTy() || !v2.getType()->isPointerTy())
    return llvm::AliasResult::NoAlias;

  return llvm::AliasResult::NoAlias;
}

llvm::AliasResult
pdg::DataDependencyGraph::queryAliasOverApproximate(llvm::Value &v1,
                                                    llvm::Value &v2) {
  // Use the over-approximation wrapper (Andersen's analysis)
  // This integrates precise pointer analysis from lib/Alias/SparrowAA
  if (_alias_wrapper_over && _alias_wrapper_over->isInitialized()) {
    return _alias_wrapper_over->query(&v1, &v2);
  }

  // Wrapper disabled or failed to initialize: stay conservative.
  return llvm::AliasResult::MayAlias;
}

void pdg::DataDependencyGraph::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<MemoryDependenceWrapperPass>();
  AU.setPreservesAll();
}

static RegisterPass<pdg::DataDependencyGraph>
    DDG("ddg", "Data Dependency Graph Construction", false, true);
