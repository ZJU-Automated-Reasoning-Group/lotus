/**
 * @file lotus-alias-fspta.cpp
 * @brief Exhaustive sparse flow-sensitive pointer-analysis driver.
 */
#include "Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/CLIUtils.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"

#include <algorithm>
#include <map>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias;
using namespace lotus::alias::tools;
using namespace lotus::analysis;

namespace {
enum class SetBackendOption { Mutable, HashConsed };
enum class PartitionOption { Distinct, IntraDisjoint, InterDisjoint };

cl::OptionCategory FsptaCategory("Lotus exhaustive flow-sensitive PTA");
cl::opt<std::string> InputFilename(cl::Positional,
                                   cl::desc("<input bitcode file>"),
                                   cl::Required, cl::cat(FsptaCategory));
cl::opt<SetBackendOption> SetBackend(
    "points-to-sets", cl::desc("Points-to set backend"),
    cl::values(clEnumValN(SetBackendOption::Mutable, "mutable",
                          "Mutable ordered sets (default)"),
               clEnumValN(SetBackendOption::HashConsed, "hash-consed",
                          "Interned immutable sets with operation caching")),
    cl::init(SetBackendOption::Mutable), cl::cat(FsptaCategory));
cl::opt<PartitionOption> MemoryPartition(
    "memory-partition", cl::desc("MemorySSA region partition"),
    cl::values(clEnumValN(PartitionOption::Distinct, "distinct",
                          "Exact points-to regions"),
               clEnumValN(PartitionOption::IntraDisjoint, "intra-disjoint",
                          "Disjoint regions per function"),
               clEnumValN(PartitionOption::InterDisjoint, "inter-disjoint",
                          "Disjoint whole-program regions")),
    cl::init(PartitionOption::InterDisjoint), cl::cat(FsptaCategory));
cl::opt<bool> PrintPointsTo("print-pts",
                            cl::desc("Print top-level points-to results"),
                            cl::init(false), cl::cat(FsptaCategory));
cl::opt<bool> PrintMemory("print-memory",
                          cl::desc("Print non-empty sparse memory facts"),
                          cl::init(false), cl::cat(FsptaCategory));
cl::opt<bool> DumpStats("dump-stats", cl::desc("Print solver statistics"),
                        cl::init(true), cl::cat(FsptaCategory));
cl::opt<std::string>
    DumpSVFG("dump-svfg", cl::desc("Write the initialized SVFG as a DOT file"),
             cl::init(""), cl::cat(FsptaCategory));
cl::opt<bool> ValidateAnnotations(
    "validate-annotations",
    cl::desc("Validate __aser_alias__/__aser_no_alias__ calls"),
    cl::init(false), cl::cat(FsptaCategory));

MemoryRegionPartitionStrategy partitionStrategy() {
  switch (MemoryPartition) {
  case PartitionOption::Distinct:
    return MemoryRegionPartitionStrategy::Distinct;
  case PartitionOption::IntraDisjoint:
    return MemoryRegionPartitionStrategy::IntraDisjoint;
  case PartitionOption::InterDisjoint:
    return MemoryRegionPartitionStrategy::InterDisjoint;
  }
  llvm_unreachable("invalid memory partition");
}

void printValue(const Value *value, raw_ostream &os) {
  if (!value)
    os << "<unknown>";
  else if (const auto *instruction = dyn_cast<Instruction>(value)) {
    if (instruction->getFunction())
      os << instruction->getFunction()->getName() << "::";
    if (value->hasName())
      os << value->getName();
    else
      value->printAsOperand(os, false);
  } else if (value->hasName())
    os << value->getName();
  else
    value->printAsOperand(os, false);
}
} // namespace

int main(int argc, char **argv) {
  InitLLVM init(argc, argv);
  cl::HideUnrelatedOptions(FsptaCategory);
  cl::ParseCommandLineOptions(argc, argv,
                              "Lotus exhaustive sparse flow-sensitive PTA\n");
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module =
      loadIRModule(InputFilename, context, diagnostic, argv[0]);
  if (!module)
    return 1;

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  graphConfig.memoryPartition = partitionStrategy();
  auto graphBuilder = std::make_unique<SVFGBuilder>(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder->build(&icfg));
  graphBuilder->connectPreAnalysisIndirectCalls(graph.get());
  if (!DumpSVFG.empty())
    graph->dump(DumpSVFG);

  FlowSensitivePTA::Config solverConfig;
  solverConfig.setBackend = SetBackend == SetBackendOption::HashConsed
                                ? PointsToSetBackend::HashConsed
                                : PointsToSetBackend::Mutable;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    const std::vector<const Function *> allowed =
        graphBuilder->getIndirectCallTargets(callSite);
    if (std::find(allowed.begin(), allowed.end(), target) == allowed.end())
      return false;
    std::vector<SVFGEdge *> edges;
    return graphBuilder->connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                         target, edges);
  };
  FlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();

  if (PrintPointsTo) {
    for (const auto &[value, nodeID] : graph->getValueNodeMap()) {
      if (!value || !value->getType()->isPointerTy())
        continue;
      const auto result = solver.pointsTo(value);
      if (!result)
        continue;
      outs() << "pts(";
      printValue(value, outs());
      outs() << ") = {";
      bool first = true;
      for (uint32_t object : *result) {
        if (!first)
          outs() << ", ";
        first = false;
        outs() << object << ":";
        printValue(graph->getObjectValue(object), outs());
      }
      outs() << "}\n";
    }
  }
  if (PrintMemory) {
    for (const auto &[nodeID, node] : *graph) {
      bool printedNode = false;
      std::set<uint32_t> flowTargets;
      std::map<uint32_t, std::string> objects(
          graph->getObjectDebugMap().begin(), graph->getObjectDebugMap().end());
      if (const auto *instruction =
              dyn_cast_or_null<Instruction>(node->getValue())) {
        const Value *pointer = nullptr;
        if (const auto *load = dyn_cast<LoadInst>(instruction))
          pointer = load->getPointerOperand();
        else if (const auto *store = dyn_cast<StoreInst>(instruction))
          pointer = store->getPointerOperand();
        if (pointer)
          if (auto targets = solver.pointsTo(pointer))
            for (uint32_t object : *targets)
              flowTargets.insert(object);
      }
      for (const auto &[object, label] : objects) {
        const auto &in = solver.memoryIn(node, object);
        const auto &out = solver.memoryOut(node, object);
        if (in.empty() && out.empty() && flowTargets.count(object) == 0)
          continue;
        if (!printedNode) {
          outs() << "memory(N" << nodeID
                 << ",kind=" << static_cast<unsigned>(node->getNodeKind());
          if (node->getValue()) {
            outs() << ",value=";
            if (const auto *instruction =
                    dyn_cast<Instruction>(node->getValue()))
              instruction->print(outs());
            else
              printValue(node->getValue(), outs());
          }
          outs() << ")\n";
          printedNode = true;
        }
        outs() << "  obj " << object << " [" << label
               << (graph->isConstantObject(object) ? ",constant" : "")
               << ",base="
               << (graph->getObjectInfo(object)
                       ? graph->getObjectInfo(object)->baseObjId
                       : 0)
               << "] in={";
        bool first = true;
        for (uint32_t target : in) {
          if (!first)
            outs() << ",";
          first = false;
          outs() << target;
        }
        outs() << "} out={";
        first = true;
        for (uint32_t target : out) {
          if (!first)
            outs() << ",";
          first = false;
          outs() << target;
        }
        outs() << "}\n";
      }
    }
  }
  if (DumpStats) {
    outs() << "fspta.nodes=" << stats.nodes << "\n"
           << "fspta.sccs=" << stats.sccs << "\n"
           << "fspta.max-scc-size=" << stats.maxSccSize << "\n"
           << "fspta.node-processes=" << stats.nodeProcesses << "\n"
           << "fspta.top-level-facts=" << stats.topLevelFacts << "\n"
           << "fspta.memory-in-facts=" << stats.memoryInFacts << "\n"
           << "fspta.memory-out-facts=" << stats.memoryOutFacts << "\n"
           << "fspta.strong-updates=" << stats.strongUpdates << "\n"
           << "fspta.weak-updates=" << stats.weakUpdates << "\n"
           << "fspta.strong-update-executions="
           << stats.strongUpdateExecutions << "\n"
           << "fspta.weak-update-executions=" << stats.weakUpdateExecutions
           << "\n"
           << "fspta.indirect-call-edges=" << stats.indirectCallEdges << "\n"
           << "fspta.hash-consed-sets=" << stats.hashConsedUniqueSets << "\n";
  }
  std::size_t validations = 0;
  std::size_t failures = 0;
  if (ValidateAnnotations) {
    for (const Function &function : *module) {
      for (const Instruction &instruction : instructions(function)) {
        const auto *call = dyn_cast<CallBase>(&instruction);
        const Function *callee = call ? call->getCalledFunction() : nullptr;
        if (!callee || call->arg_size() < 2)
          continue;
        const bool expectAlias = callee->getName() == "__aser_alias__";
        const bool expectNoAlias = callee->getName() == "__aser_no_alias__";
        if (!expectAlias && !expectNoAlias)
          continue;
        ++validations;
        const auto result =
            solver.mayAlias(call->getArgOperand(0), call->getArgOperand(1));
        if (!result || *result != expectAlias) {
          ++failures;
          errs() << InputFilename << ": annotation mismatch in "
                 << function.getName() << ": expected "
                 << (expectAlias ? "alias" : "no-alias") << ", got ";
          if (!result)
            errs() << "unresolved";
          else
            errs() << (*result ? "alias" : "no-alias");
          errs() << " for ";
          printValue(call->getArgOperand(0), errs());
          errs() << " and ";
          printValue(call->getArgOperand(1), errs());
          errs() << "\n";
        }
      }
    }
    outs() << "fspta.validations=" << validations << "\n"
           << "fspta.validation-failures=" << failures << "\n";
  }
  return failures == 0 ? 0 : 2;
}
