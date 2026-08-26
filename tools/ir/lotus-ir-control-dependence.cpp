//===- lotus-ir-control-dependence.cpp - Control-dependence driver --------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include "Analysis/ControlDependence/CompactControlDependence.h"
#include "Analysis/ControlDependence/ControlClosure.h"
#include "Analysis/ControlDependence/DOD.h"
#include "Analysis/ControlDependence/NTSCD.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;
using namespace lotus::cd::detail;

namespace {

cl::opt<std::string> InputFilename(cl::Positional,
                                   cl::desc("<input LLVM IR/bitcode>"),
                                   cl::Required);
cl::opt<std::string> AlgorithmName(
    "algorithm",
    cl::desc("ntscd2, ntscd-compact, dod, dod-compact, dod-ntscd, "
             "dod-ntscd-compact, strong-closure, compact-closure"),
    cl::init("dod-compact"));
cl::opt<bool> VisitPairs(
    "visit-pairs",
    cl::desc("Visit exact DOD pairs with a counting callback; never print or "
             "store individual pairs"),
    cl::init(false));
cl::opt<std::string> FunctionName("function",
                                  cl::desc("Analyze only this function"),
                                  cl::init(""));
cl::list<unsigned> SeedIndices(
    "seed-index",
    cl::desc("Extra zero-based closure seed; entry is always included"),
    cl::ZeroOrMore);
cl::opt<std::string> Format("format", cl::desc("text, json, or csv"),
                            cl::init("text"));

enum class Algorithm {
  NTSCD2,
  NTSCDCompact,
  DOD,
  DODCompact,
  DODNTSCD,
  DODNTSCDCompact,
  StrongClosure,
  CompactClosure,
};

Algorithm parseAlgorithm(StringRef name) {
  if (name == "ntscd2")
    return Algorithm::NTSCD2;
  if (name == "ntscd-compact")
    return Algorithm::NTSCDCompact;
  if (name == "dod")
    return Algorithm::DOD;
  if (name == "dod-compact")
    return Algorithm::DODCompact;
  if (name == "dod-ntscd")
    return Algorithm::DODNTSCD;
  if (name == "dod-ntscd-compact")
    return Algorithm::DODNTSCDCompact;
  if (name == "strong-closure")
    return Algorithm::StrongClosure;
  if (name == "compact-closure")
    return Algorithm::CompactClosure;
  report_fatal_error(Twine("unknown algorithm: ") + name);
}

StringRef nameOf(Algorithm algorithm) {
  switch (algorithm) {
  case Algorithm::NTSCD2:
    return "ntscd2";
  case Algorithm::NTSCDCompact:
    return "ntscd-compact";
  case Algorithm::DOD:
    return "dod";
  case Algorithm::DODCompact:
    return "dod-compact";
  case Algorithm::DODNTSCD:
    return "dod-ntscd";
  case Algorithm::DODNTSCDCompact:
    return "dod-ntscd-compact";
  case Algorithm::StrongClosure:
    return "strong-closure";
  case Algorithm::CompactClosure:
    return "compact-closure";
  }
  llvm_unreachable("unknown algorithm");
}

bool isDOD(Algorithm a) {
  return a == Algorithm::DOD || a == Algorithm::DODCompact;
}
bool isClosure(Algorithm a) {
  return a == Algorithm::StrongClosure || a == Algorithm::CompactClosure;
}

struct FunctionGraph {
  Graph graph;
  std::vector<const BasicBlock *> blocks;
  size_t edges{0};

  explicit FunctionGraph(Function &function) : graph(function.getName().str()) {
    DenseMap<const BasicBlock *, GraphNode *> map;
    for (BasicBlock &block : function) {
      blocks.push_back(&block);
      map[&block] = &graph.createNode();
    }
    for (BasicBlock &block : function)
      for (BasicBlock *successor : successors(&block)) {
        size_t before = map[&block]->successors().size();
        graph.addEdge(*map[&block], *map[successor]);
        edges += before != map[&block]->successors().size();
      }
  }

  NodeSet seed() {
    NodeSet result;
    if (!blocks.empty())
      result.insert(graph.getNode(1));
    for (unsigned index : SeedIndices) {
      if (index >= blocks.size())
        report_fatal_error("seed index outside function " +
                           blocks.front()->getParent()->getName());
      result.insert(graph.getNode(index + 1));
    }
    return result;
  }
};

size_t relationSize(const DependenceResult &r) {
  size_t result = 0;
  for (const auto &entry : r.first)
    result += entry.second.size();
  return result;
}

void merge(DependenceResult &to, const DependenceResult &from) {
  for (const auto &entry : from.first)
    to.first[entry.first].insert(entry.second.begin(), entry.second.end());
  for (const auto &entry : from.second)
    to.second[entry.first].insert(entry.second.begin(), entry.second.end());
}

using Clock = std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                              start)
      .count();
}

struct Record {
  std::string function;
  std::string algorithm;
  size_t nodes{}, edges{}, decisions{}, dependencies{};
  size_t bicliques{}, incidences{}, pairs{}, closureSize{};
  uint64_t totalNS{}, inevitableNS{}, ntscdNS{}, dodNS{}, visitNS{},
      closureNS{};
};

Record run(Function &function, FunctionGraph &fg, Algorithm algorithm) {
  Record r;
  r.function = function.getName().str();
  r.algorithm = nameOf(algorithm).str();
  r.nodes = fg.graph.size();
  r.edges = fg.edges;
  r.decisions = fg.graph.predicates().size();
  DependenceResult deps, ntscd;
  DODBicliqueMap bicliques;
  NodeSet closure;
  auto totalStart = Clock::now();

  switch (algorithm) {
  case Algorithm::NTSCD2:
    deps = computeNTSCD2(fg.graph);
    break;
  case Algorithm::NTSCDCompact: {
    auto start = Clock::now();
    Inevitability inevitable = computeInevitability(fg.graph);
    r.inevitableNS = elapsed(start);
    start = Clock::now();
    deps = computeCompactNTSCD(fg.graph, inevitable);
    r.ntscdNS = elapsed(start);
    break;
  }
  case Algorithm::DOD:
    if (VisitPairs) {
      auto start = Clock::now();
      forEachBaselineDODPair(
          fg.graph, [&](GraphNode *, GraphNode *, GraphNode *) { ++r.pairs; });
      r.visitNS = elapsed(start);
    } else {
      r.bicliques = preprocessBaselineDOD(fg.graph);
    }
    break;
  case Algorithm::DODCompact: {
    auto start = Clock::now();
    Inevitability inevitable = computeInevitability(fg.graph);
    r.inevitableNS = elapsed(start);
    start = Clock::now();
    bicliques = computeCompactDOD(fg.graph, inevitable);
    r.dodNS = elapsed(start);
    if (VisitPairs) {
      start = Clock::now();
      forEachDODPair(fg.graph, bicliques,
                     [&](GraphNode *, GraphNode *, GraphNode *) { ++r.pairs; });
      r.visitNS = elapsed(start);
    }
    break;
  }
  case Algorithm::DODNTSCD:
    deps = computeDODNTSCD(fg.graph);
    break;
  case Algorithm::DODNTSCDCompact: {
    auto start = Clock::now();
    Inevitability inevitable = computeInevitability(fg.graph);
    r.inevitableNS = elapsed(start);
    start = Clock::now();
    ntscd = computeCompactNTSCD(fg.graph, inevitable);
    r.ntscdNS = elapsed(start);
    start = Clock::now();
    bicliques = computeCompactDOD(fg.graph, inevitable);
    r.dodNS = elapsed(start);
    deps = ntscd;
    merge(deps, materializeCompactDODDependencies(fg.graph, bicliques));
    break;
  }
  case Algorithm::StrongClosure: {
    auto start = Clock::now();
    closure = computeStrongControlClosure(fg.graph, fg.seed());
    r.closureNS = elapsed(start);
    break;
  }
  case Algorithm::CompactClosure: {
    auto start = Clock::now();
    Inevitability inevitable = computeInevitability(fg.graph);
    r.inevitableNS = elapsed(start);
    start = Clock::now();
    ntscd = computeCompactNTSCD(fg.graph, inevitable);
    r.ntscdNS = elapsed(start);
    start = Clock::now();
    bicliques = computeCompactDOD(fg.graph, inevitable);
    r.dodNS = elapsed(start);
    start = Clock::now();
    closure =
        computeCompactDependencyClosure(fg.graph, fg.seed(), ntscd, bicliques);
    r.closureNS = elapsed(start);
    break;
  }
  }

  r.totalNS = elapsed(totalStart);
  r.dependencies = relationSize(deps);
  if (!bicliques.empty()) {
    r.bicliques = bicliques.size();
    for (const auto &entry : bicliques) {
      r.incidences += entry.second.left.count() + entry.second.right.count();
      if (!VisitPairs)
        r.pairs += entry.second.pairCount();
    }
  }
  r.closureSize = closure.size();
  return r;
}

void printText(const std::vector<Record> &records) {
  for (const Record &r : records)
    outs() << r.function << " " << r.algorithm << " nodes=" << r.nodes
           << " edges=" << r.edges << " decisions=" << r.decisions
           << " dependencies=" << r.dependencies << " bicliques=" << r.bicliques
           << " incidences=" << r.incidences << " dod_pairs=" << r.pairs
           << " closure_size=" << r.closureSize << " analysis_ns=" << r.totalNS
           << " inevitability_ns=" << r.inevitableNS
           << " ntscd_ns=" << r.ntscdNS << " dod_ns=" << r.dodNS
           << " pair_visit_ns=" << r.visitNS << " closure_ns=" << r.closureNS
           << "\n";
}

void printCSV(const std::vector<Record> &records) {
  outs() << "function,algorithm,nodes,edges,decisions,dependencies,bicliques,"
            "incidences,dod_pairs,closure_size,analysis_ns,inevitability_ns,"
            "ntscd_ns,dod_ns,pair_visit_ns,closure_ns\n";
  for (const Record &r : records)
    outs() << '"' << r.function << "\"," << r.algorithm << ',' << r.nodes << ','
           << r.edges << ',' << r.decisions << ',' << r.dependencies << ','
           << r.bicliques << ',' << r.incidences << ',' << r.pairs << ','
           << r.closureSize << ',' << r.totalNS << ',' << r.inevitableNS << ','
           << r.ntscdNS << ',' << r.dodNS << ',' << r.visitNS << ','
           << r.closureNS << '\n';
}

void printJSON(const std::vector<Record> &records) {
  json::Array array;
  for (const Record &r : records) {
    json::Object o;
    o["function"] = r.function;
    o["algorithm"] = r.algorithm;
    o["nodes"] = int64_t(r.nodes);
    o["edges"] = int64_t(r.edges);
    o["decisions"] = int64_t(r.decisions);
    o["dependencies"] = int64_t(r.dependencies);
    o["bicliques"] = int64_t(r.bicliques);
    o["incidences"] = int64_t(r.incidences);
    o["dod_pairs"] = int64_t(r.pairs);
    o["closure_size"] = int64_t(r.closureSize);
    o["analysis_ns"] = int64_t(r.totalNS);
    o["inevitability_ns"] = int64_t(r.inevitableNS);
    o["ntscd_ns"] = int64_t(r.ntscdNS);
    o["dod_ns"] = int64_t(r.dodNS);
    o["pair_visit_ns"] = int64_t(r.visitNS);
    o["closure_ns"] = int64_t(r.closureNS);
    array.push_back(std::move(o));
  }
  outs() << formatv("{0:2}\n", json::Value(std::move(array)));
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM init(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "Lotus control-dependence algorithm driver\n");
  if (Format != "text" && Format != "json" && Format != "csv")
    report_fatal_error("--format must be text, json, or csv");
  Algorithm algorithm = parseAlgorithm(AlgorithmName);
  if (VisitPairs && !isDOD(algorithm))
    report_fatal_error("--visit-pairs is valid only for dod and dod-compact");
  if (!SeedIndices.empty() && !isClosure(algorithm))
    report_fatal_error("--seed-index is valid only for closure algorithms");

  LLVMContext context;
  SMDiagnostic diagnostic;
  auto module = parseIRFile(InputFilename, diagnostic, context);
  if (!module) {
    diagnostic.print(argv[0], errs());
    return 1;
  }
  std::vector<Record> records;
  bool found = FunctionName.empty();
  for (Function &function : *module) {
    if (function.isDeclaration() || function.empty())
      continue;
    if (!FunctionName.empty() && function.getName() != FunctionName)
      continue;
    found = true;
    FunctionGraph graph(function);
    records.push_back(run(function, graph, algorithm));
  }
  if (!found)
    report_fatal_error(Twine("function not found: ") + FunctionName.getValue());
  if (Format == "json")
    printJSON(records);
  else if (Format == "csv")
    printCSV(records);
  else
    printText(records);
}
