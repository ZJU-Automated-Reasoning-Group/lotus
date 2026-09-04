/**
 * @file llvm-ai.cpp
 * @brief LLVM IFDS/IDE Analysis Tool
 *
 * A command-line tool for running IFDS/IDE interprocedural dataflow analysis
 */

#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/Subcommands.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"
#include "Utils/LLVM/Demangle.h"

#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h>
#include <Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h>
#include <Dataflow/IFDS/Core/IFDSFramework.h>
#include <Dataflow/IFDS/Solver/IFDSSolver.h>

// #include <iostream>
// #include <thread>

using namespace llvm;
using namespace ifds;

enum class TaintAASelection {
  Andersen,
  Dyck,
  CFLAnders,
  CFLSteens,
  SeaDsa,
  AllocAA,
  BasicAA,
  Combined,
};

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::Required,
                  cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<TaintAASelection> AliasAnalysisType(
    "taint.alias-analysis", cl::desc("Alias analysis type"),
    cl::values(
        clEnumValN(TaintAASelection::Andersen, "andersen", "Andersen analysis"),
        clEnumValN(TaintAASelection::Dyck, "dyck", "DyckAA (default)"),
        clEnumValN(TaintAASelection::CFLAnders, "cfl-anders", "CFL-Anders"),
        clEnumValN(TaintAASelection::CFLSteens, "cfl-steens", "CFL-Steens"),
        clEnumValN(TaintAASelection::SeaDsa, "sea-dsa", "SeaDsa"),
        clEnumValN(TaintAASelection::AllocAA, "alloc-aa", "Allocation-site AA"),
        clEnumValN(TaintAASelection::BasicAA, "basic-aa", "LLVM BasicAA"),
        clEnumValN(TaintAASelection::Combined, "combined",
                   "Andersen (context-insensitive) and DyckAA")),
    cl::init(TaintAASelection::Dyck),
    cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string> SourceFunctions(
    "taint.sources", cl::desc("Comma-separated list of source functions"),
    cl::init(""), cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string> SinkFunctions(
    "taint.sinks", cl::desc("Comma-separated list of sink functions"),
    cl::init(""), cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<bool>
    MicroBench("taint.micro-bench",
               cl::desc("Enable micro benchmark mode (use source/sink and "
                        "evaluate precision/recall)"),
               cl::init(false),
               cl::sub(lotus::checker::tooling::taintSubCommand()));

static cl::opt<std::string> ExpectedFile(
    "taint.expected-flows",
    cl::desc("Path to .expected file for micro benchmark evaluation"),
    cl::init(""), cl::sub(lotus::checker::tooling::taintSubCommand()));

// Helper function to parse comma-separated function names
std::vector<std::string> parseFunctionList(const std::string &input) {
  std::vector<std::string> functions;
  if (input.empty())
    return functions;

  std::stringstream ss(input);
  std::string item;
  while (std::getline(ss, item, ',')) {
    StringRef trimmed = StringRef(item).trim();
    if (!trimmed.empty()) {
      functions.push_back(trimmed.str());
    }
  }
  return functions;
}

using TaintFlow = std::pair<std::string, std::string>;

static std::optional<std::string> resolveCalleeName(const CallBase &call);

static ErrorOr<std::set<TaintFlow>> loadExpectedFlows(StringRef filename) {
  auto bufferOr = MemoryBuffer::getFile(filename);
  if (!bufferOr) {
    return bufferOr.getError();
  }

  std::set<TaintFlow> flows;
  SmallVector<StringRef, 32> lines;
  bufferOr.get()->getBuffer().split(lines, '\n');
  for (size_t index = 0; index < lines.size(); ++index) {
    StringRef line = lines[index].split('#').first.trim();
    if (line.empty()) {
      continue;
    }

    StringRef source;
    StringRef sink;
    size_t arrow = line.find("->");
    if (arrow != StringRef::npos) {
      source = line.take_front(arrow).trim();
      sink = line.drop_front(arrow + 2).trim();
    } else {
      auto parts = line.split(',');
      source = parts.first.trim();
      sink = parts.second.trim();
    }
    if (source.empty() || sink.empty() || sink.contains(',')) {
      errs() << "error: invalid expected flow at " << filename << ":"
             << (index + 1) << " (expected source->sink)\n";
      return std::make_error_code(std::errc::invalid_argument);
    }
    flows.emplace(source.str(), sink.str());
  }
  return flows;
}

static std::set<TaintFlow>
collectDetectedFlows(const TaintAnalysis &analysis,
                     const IFDSSolver<TaintAnalysis> &solver) {
  std::set<TaintFlow> flows;
  for (const auto &entry : solver.get_all_results()) {
    const auto &node = entry.first;
    const auto &facts = entry.second;
    const auto *sinkCall = dyn_cast_or_null<CallBase>(node.instruction);
    if (!sinkCall || !analysis.is_sink(sinkCall)) {
      continue;
    }

    auto sinkName = resolveCalleeName(*sinkCall);
    if (!sinkName) {
      continue;
    }

    for (const TaintFact &fact : facts) {
      bool reachesArgument = false;
      for (const Use &argument : sinkCall->args()) {
        if (analysis.is_argument_tainted(argument.get(), fact)) {
          reachesArgument = true;
          break;
        }
      }
      if (!reachesArgument) {
        continue;
      }

      const auto *directSource = dyn_cast_or_null<CallBase>(fact.get_source());
      if (directSource) {
        auto sourceName = resolveCalleeName(*directSource);
        if (sourceName) {
          flows.emplace(*sourceName, *sinkName);
          continue;
        }
      }

      auto path =
          analysis.trace_taint_sources_summary_based(solver, sinkCall, fact);
      for (const Instruction *sourceInst : path.sources) {
        const auto *sourceCall = dyn_cast_or_null<CallBase>(sourceInst);
        if (!sourceCall) {
          continue;
        }
        auto sourceName = resolveCalleeName(*sourceCall);
        if (sourceName) {
          flows.emplace(*sourceName, *sinkName);
        }
      }
    }
  }
  return flows;
}

static std::optional<std::string> resolveCalleeName(const CallBase &call) {
  if (const Function *callee = call.getCalledFunction()) {
    return callee->getName().str();
  }

  const Value *called = call.getCalledOperand()->stripPointerCasts();
  if (const auto *callee = dyn_cast<Function>(called)) {
    return callee->getName().str();
  }
  return std::nullopt;
}

static std::string describeCallee(const CallBase &call) {
  if (auto callee = resolveCalleeName(call)) {
    return *callee;
  }

  std::string called;
  raw_string_ostream os(called);
  call.getCalledOperand()->print(os);
  return "indirect call through " + os.str();
}

static int emitTaintReports(const TaintAnalysis &analysis,
                            const IFDSSolver<TaintAnalysis> &solver) {
  BugReportMgr &manager = BugReportMgr::get_instance();
  const int bugTypeId = manager.register_bug_type(
      "Taint-Style Vulnerability", BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY,
      "CWE-15, CWE-23, CWE-78, CWE-90, CWE-123, CWE-256, CWE-319, CWE-426, "
      "CWE-427, CWE-591");

  for (const auto &entry : solver.get_all_results()) {
    const auto *sinkCall = dyn_cast_or_null<CallBase>(entry.first.instruction);
    if (!sinkCall || !analysis.is_sink(sinkCall) || entry.second.empty()) {
      continue;
    }

    std::string taintedArgs;
    analysis.analyze_tainted_arguments(sinkCall, entry.second, taintedArgs);
    if (taintedArgs.empty()) {
      continue;
    }

    auto *report = new BugReport(bugTypeId);
    SmallPtrSet<const Instruction *, 8> seenSources;
    unsigned traceLevel = 0;
    for (const TaintFact &fact : entry.second) {
      bool reachesArgument = false;
      for (const Use &argument : sinkCall->args()) {
        if (analysis.is_argument_tainted(argument.get(), fact)) {
          reachesArgument = true;
          break;
        }
      }
      if (!reachesArgument) {
        continue;
      }

      const Instruction *source = fact.get_source();
      if (source && seenSources.insert(source).second) {
        report->append_step(const_cast<Instruction *>(source),
                            "Taint originates here", traceLevel++, {},
                            "source");
      }

      auto path =
          analysis.trace_taint_sources_summary_based(solver, sinkCall, fact);
      for (const Instruction *pathSource : path.sources) {
        if (pathSource && seenSources.insert(pathSource).second) {
          report->append_step(const_cast<Instruction *>(pathSource),
                              "Taint originates here", traceLevel++, {},
                              "source");
        }
      }
    }

    report->append_step(const_cast<CallBase *>(sinkCall),
                        "Tainted data reaches sink '" +
                            describeCallee(*sinkCall) + "' via " + taintedArgs,
                        traceLevel, {NodeTag::CALL_SITE}, "sink");
    report->set_conf_score(80);
    report->set_suggestion(
        "Validate or sanitize untrusted input before passing it to this sink");
    manager.insert_report(bugTypeId, report, true);
  }

  lotus::checker::tooling::CheckerReportOptions reportOptions;
  reportOptions.verbose = lotus::checker::tooling::Verbose;
  reportOptions.printText = true;
  return lotus::checker::tooling::emitCheckerReports(manager, reportOptions);
}

static void evaluateMicroBenchmark(const std::set<TaintFlow> &expected,
                                   const std::set<TaintFlow> &detected,
                                   raw_ostream &OS) {
  size_t truePositives = 0;
  for (const TaintFlow &flow : detected) {
    truePositives += expected.count(flow);
  }
  const size_t falsePositives = detected.size() - truePositives;
  const size_t falseNegatives = expected.size() - truePositives;
  const double precision =
      detected.empty() ? (expected.empty() ? 1.0 : 0.0)
                       : static_cast<double>(truePositives) / detected.size();
  const double recall =
      expected.empty() ? 1.0
                       : static_cast<double>(truePositives) / expected.size();
  const double f1 = precision + recall == 0.0
                        ? 0.0
                        : 2.0 * precision * recall / (precision + recall);

  OS << "\nMicro-benchmark evaluation:\n"
     << "  TP: " << truePositives << "  FP: " << falsePositives
     << "  FN: " << falseNegatives << "\n"
     << "  Precision: " << precision << "\n"
     << "  Recall: " << recall << "\n"
     << "  F1: " << f1 << "\n";
}

static void dumpSourceSinkMatches(const llvm::Module &module,
                                  const TaintAnalysis &analysis,
                                  llvm::raw_ostream &OS) {
  size_t total_calls = 0;
  size_t source_calls = 0;
  size_t sink_calls = 0;

  auto demangle_name = [](const std::string &name) {
    return DemangleUtils::demangle(name);
  };

  OS << "\nDetected call sites (source/sink tagging):\n";
  OS << "=========================================\n";

  for (const auto &function : module) {
    for (const auto &inst : instructions(function)) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (!call)
        continue;

      ++total_calls;
      bool is_source = analysis.is_source(call);
      bool is_sink = analysis.is_sink(call);
      if (is_source)
        ++source_calls;
      if (is_sink)
        ++sink_calls;

      auto raw_name = describeCallee(*call);
      auto demangled_name = demangle_name(raw_name);
      const DebugLoc debugLocation = call->getDebugLoc();
      const unsigned line = debugLocation ? debugLocation.getLine() : 0;

      OS << "  ";
      if (is_source)
        OS << "[source] ";
      if (is_sink)
        OS << "[sink] ";
      if (!is_source && !is_sink)
        OS << "[ ] ";
      OS << raw_name;
      if (demangled_name != raw_name) {
        OS << " -> " << demangled_name;
      }
      if (line > 0) {
        OS << " @ line " << line;
      }
      OS << "\n";
    }
  }

  OS << "Summary: " << total_calls << " calls, " << source_calls << " sources, "
     << sink_calls << " sinks\n";
}

static bool validateConfiguredFunctions(const llvm::Module &module,
                                        ArrayRef<std::string> names,
                                        StringRef kind) {
  bool valid = true;
  for (const std::string &name : names) {
    size_t matches = 0;
    for (const Function &function : module) {
      for (const Instruction &inst : instructions(function)) {
        if (TaintAnalysis::matches_function_name(&inst, name)) {
          ++matches;
        }
      }
    }
    if (matches == 0) {
      errs() << "error: " << kind << " function '" << name
             << "' matched 0 call sites\n";
      valid = false;
    }
  }
  return valid;
}

static lotus::AAConfig getAliasAnalysisConfig(TaintAASelection selection) {
  switch (selection) {
  case TaintAASelection::Andersen:
    return lotus::AAConfig::SparrowAA_NoCtx();
  case TaintAASelection::Dyck:
    return lotus::AAConfig::DyckAA();
  case TaintAASelection::CFLAnders:
    return lotus::AAConfig::CFLAnders();
  case TaintAASelection::CFLSteens:
    return lotus::AAConfig::CFLSteens();
  case TaintAASelection::SeaDsa:
    return lotus::AAConfig::SeaDsa();
  case TaintAASelection::AllocAA:
    return lotus::AAConfig::AllocAA();
  case TaintAASelection::BasicAA:
    return lotus::AAConfig::BasicAA();
  case TaintAASelection::Combined:
    return lotus::AAConfig::Combined();
  }
  llvm_unreachable("invalid taint alias analysis selection");
}

int runTaintCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::Taint);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Set up LLVM context and source manager
  LLVMContext Context;
  SMDiagnostic Err;

  // Load the input module
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("taint", *M, mgr);

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Debug)) {
    outs() << "Loaded module: " << M->getName() << "\n";
    outs() << "Functions in module: " << M->size() << "\n";
  }

  auto sources = parseFunctionList(SourceFunctions);
  auto sinks = parseFunctionList(SinkFunctions);
  if (MicroBench) {
    sources.push_back("source");
    sinks.push_back("sink");
  }

  const bool sourcesValid = validateConfiguredFunctions(*M, sources, "source");
  const bool sinksValid = validateConfiguredFunctions(*M, sinks, "sink");
  if (!sourcesValid || !sinksValid) {
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Set up alias analysis wrapper
  lotus::AAConfig aaConfig =
      getAliasAnalysisConfig(AliasAnalysisType.getValue());
  auto aliasWrapper =
      std::make_unique<lotus::AliasAnalysisWrapper>(*M, aaConfig);

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Debug)) {
    outs() << "Using alias analysis: "
           << lotus::AliasAnalysisFactory::getTypeName(aaConfig) << "\n";
  }

  if (!aliasWrapper->isInitialized()) {
    errs() << "error: alias analysis failed to initialize\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Run the taint analysis
  try {
    {
      if (lotus::checker::tooling::logAtLeast(
              lotus::checker::tooling::LogLevel::Info))
        outs() << "Running interprocedural taint analysis...\n";

      TaintAnalysis taintAnalysis;

      for (const auto &source : sources) {
        taintAnalysis.add_source_function(source);
      }
      for (const auto &sink : sinks) {
        taintAnalysis.add_sink_function(sink);
      }

      // Set up alias analysis
      taintAnalysis.set_alias_analysis(aliasWrapper.get());

      if (lotus::checker::tooling::logAtLeast(
              lotus::checker::tooling::LogLevel::Debug)) {
        dumpSourceSinkMatches(*M, taintAnalysis, outs());
      }

      if (lotus::checker::tooling::logAtLeast(
              lotus::checker::tooling::LogLevel::Info))
        outs() << "Using sequential IFDS solver\n";

      ifds::IFDSSolver<ifds::TaintAnalysis> solver(taintAnalysis);

      // Enable progress bar when running in verbose mode
      if (lotus::checker::tooling::logAtLeast(
              lotus::checker::tooling::LogLevel::Debug)) {
        auto config = solver.get_solver_config();
        config.set_enable_progress_reporting(true);
        solver.set_solver_config(config);
      }

      solver.solve(*M);

      if (MicroBench) {
        SmallString<256> expectedPath;
        if (!ExpectedFile.empty()) {
          expectedPath = ExpectedFile;
        } else {
          expectedPath = InputFilename;
          sys::path::replace_extension(expectedPath, "expected");
        }
        auto expectedOr = loadExpectedFlows(expectedPath);
        if (!expectedOr) {
          errs() << "error: could not read expected flows from " << expectedPath
                 << ": " << expectedOr.getError().message() << "\n";
          return lotus::checker::tooling::EXIT_ERROR;
        }
        evaluateMicroBenchmark(
            *expectedOr, collectDetectedFlows(taintAnalysis, solver), outs());
      }
      const int reportStatus = emitTaintReports(taintAnalysis, solver);
      stats.emit();
      if (reportStatus != lotus::checker::tooling::EXIT_SUCCESS_CODE) {
        return reportStatus;
      }
    }

    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Analysis completed successfully.\n";

  } catch (const std::exception &e) {
    errs() << "Error running analysis: " << e.what() << "\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Statistics will be printed automatically at program exit if enabled

  return lotus::checker::tooling::EXIT_SUCCESS_CODE;
}
