#include "CFL/Classical/LLVMAliasAnalysis.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

using namespace lotus::cfl::classical;

namespace {

struct Options {
  std::string input;
  LLVMAliasOptions analysis;
  bool json_stats = false;
  bool print_points_to = false;
  bool check_annotations = false;
  std::string query_lhs;
  std::string query_rhs;
};

void usage(std::ostream &stream) {
  stream << "Usage: lotus-cfl-alias [options] INPUT.{ll,bc}\n"
            "Options:\n"
            "  --solver baseline|pocr|hybrid\n"
            "  --encoding pag|peg\n"
            "  --entry FUNCTION\n"
            "  --max-callgraph-rounds N\n"
            "  --query LHS,RHS\n"
            "  --print-points-to\n"
            "  --check-annotations\n"
            "  --json-stats\n";
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto value = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("Missing value for " + argument);
      }
      return argv[index];
    };
    if (argument == "--solver") {
      const std::string selected = value();
      if (selected == "baseline") {
        options.analysis.backend = SolverBackend::Baseline;
      } else if (selected == "pocr") {
        options.analysis.backend = SolverBackend::POCR;
      } else if (selected == "hybrid") {
        options.analysis.backend = SolverBackend::Hybrid;
      } else {
        throw std::invalid_argument("Unknown solver: " + selected);
      }
    } else if (argument == "--encoding") {
      const std::string selected = value();
      if (selected == "pag") {
        options.analysis.encoding = AliasEncodingMode::PAG;
      } else if (selected == "peg") {
        options.analysis.encoding = AliasEncodingMode::PEG;
      } else {
        throw std::invalid_argument("Unknown encoding: " + selected);
      }
    } else if (argument == "--entry") {
      options.analysis.entry = value();
    } else if (argument == "--max-callgraph-rounds") {
      options.analysis.max_callgraph_rounds = std::stoul(value());
    } else if (argument == "--query") {
      const std::string query = value();
      const auto comma = query.find(',');
      if (comma == std::string::npos) {
        throw std::invalid_argument("Alias query must be LHS,RHS");
      }
      options.query_lhs = query.substr(0, comma);
      options.query_rhs = query.substr(comma + 1);
    } else if (argument == "--print-points-to") {
      options.print_points_to = true;
    } else if (argument == "--check-annotations") {
      options.check_annotations = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else if (!argument.empty() && argument.front() == '-') {
      throw std::invalid_argument("Unknown option: " + argument);
    } else if (options.input.empty()) {
      options.input = argument;
    } else {
      throw std::invalid_argument("Multiple input modules were provided");
    }
  }
  if (options.input.empty()) {
    throw std::invalid_argument("An input LLVM module is required");
  }
  return options;
}

const llvm::Value *findNamedValue(const llvm::Module &module,
                                  const std::string &name) {
  if (const auto *global = module.getNamedGlobal(name)) {
    return global;
  }
  if (const auto *function = module.getFunction(name)) {
    return function;
  }
  for (const llvm::Function &function : module) {
    for (const llvm::Argument &argument : function.args()) {
      if (argument.getName() == name) {
        return &argument;
      }
    }
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      if (instruction.getName() == name) {
        return &instruction;
      }
    }
  }
  return nullptr;
}

void printPointsTo(const llvm::Module &module,
                   const LLVMCFLAliasAnalysis &analysis) {
  for (const llvm::Function &function : module) {
    for (const llvm::Argument &argument : function.args()) {
      if (!argument.getType()->isPointerTy() || !argument.hasName()) {
        continue;
      }
      std::cout << argument.getName().str() << ':';
      for (const llvm::Value *target : analysis.pointsTo(&argument)) {
        std::cout << ' '
                  << (target->hasName() ? target->getName().str()
                                        : std::string("<unnamed>"));
      }
      std::cout << '\n';
    }
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      if (!instruction.getType()->isPointerTy() || !instruction.hasName()) {
        continue;
      }
      std::cout << instruction.getName().str() << ':';
      for (const llvm::Value *target : analysis.pointsTo(&instruction)) {
        std::cout << ' '
                  << (target->hasName() ? target->getName().str()
                                        : std::string("<unnamed>"));
      }
      std::cout << '\n';
    }
  }
}

std::pair<std::size_t, std::size_t>
checkAnnotations(const llvm::Module &module,
                 const LLVMCFLAliasAnalysis &analysis) {
  std::size_t total = 0;
  std::size_t failures = 0;
  for (const llvm::Function &function : module) {
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
      const llvm::Function *callee = call ? call->getCalledFunction() : nullptr;
      if (!callee || call->arg_size() < 2) {
        continue;
      }
      const llvm::StringRef name = callee->getName();
      const bool expect_alias = name == "__aser_alias__" || name == "MAYALIAS";
      const bool expect_no_alias =
          name == "__aser_no_alias__" || name == "NOALIAS";
      if (!expect_alias && !expect_no_alias) {
        continue;
      }
      ++total;
      const bool actual =
          analysis.mayAlias(call->getArgOperand(0), call->getArgOperand(1));
      const bool passed = expect_alias ? actual : !actual;
      failures += passed ? 0 : 1;
      std::cout << "annotation=" << (passed ? "pass" : "fail")
                << " expected=" << (expect_alias ? "alias" : "no-alias")
                << " function=" << function.getName().str();
      if (!passed) {
        const auto lhs = analysis.nodeForValue(call->getArgOperand(0));
        const auto rhs = analysis.nodeForValue(call->getArgOperand(1));
        std::cout << " lhs_node="
                  << (lhs ? std::to_string(*lhs) : std::string("missing"))
                  << " rhs_node="
                  << (rhs ? std::to_string(*rhs) : std::string("missing"));
        if (lhs) {
          std::cout << " lhs_pts=";
          for (std::size_t target : analysis.client().pointsTo(*lhs)) {
            std::cout << target << ';';
          }
        }
        if (rhs) {
          std::cout << " rhs_pts=";
          for (std::size_t target : analysis.client().pointsTo(*rhs)) {
            std::cout << target << ';';
          }
        }
      }
      std::cout << '\n';
    }
  }
  return {total, failures};
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(options.input, diagnostic, context);
    if (!module) {
      diagnostic.print(argv[0], llvm::errs());
      return 2;
    }

    LLVMCFLAliasAnalysis analysis(options.analysis);
    const ReachabilityStats stats = analysis.analyze(*module);
    if (!options.query_lhs.empty()) {
      const llvm::Value *lhs = findNamedValue(*module, options.query_lhs);
      const llvm::Value *rhs = findNamedValue(*module, options.query_rhs);
      if (!lhs || !rhs) {
        throw std::invalid_argument("Alias query names were not found");
      }
      std::cout << "alias=" << (analysis.mayAlias(lhs, rhs) ? "may" : "no")
                << '\n';
    }
    if (options.print_points_to) {
      printPointsTo(*module, analysis);
    }
    std::size_t annotation_total = 0;
    std::size_t annotation_failures = 0;
    if (options.check_annotations) {
      std::tie(annotation_total, annotation_failures) =
          checkAnnotations(*module, analysis);
    }
    if (options.json_stats) {
      std::cout << "{\"solver\":\""
                << solverBackendName(options.analysis.backend)
                << "\",\"encoding\":\""
                << (options.analysis.encoding == AliasEncodingMode::PAG ? "pag"
                                                                        : "peg")
                << "\",\"nodes\":" << stats.graph_nodes
                << ",\"base_edges\":" << stats.base_graph_edges
                << ",\"relation_edges\":" << stats.relation_edges
                << ",\"start_edges\":" << stats.start_symbol_edges
                << ",\"callgraph_rounds\":" << stats.solver_rounds
                << ",\"annotation_total\":" << annotation_total
                << ",\"annotation_failures\":" << annotation_failures
                << ",\"solve_us\":" << stats.solve_time_microseconds << "}\n";
    } else {
      std::cout << "solver=" << solverBackendName(options.analysis.backend)
                << " encoding="
                << (options.analysis.encoding == AliasEncodingMode::PAG ? "pag"
                                                                        : "peg")
                << " nodes=" << stats.graph_nodes
                << " base_edges=" << stats.base_graph_edges
                << " relation_edges=" << stats.relation_edges
                << " start_edges=" << stats.start_symbol_edges
                << " callgraph_rounds=" << stats.solver_rounds
                << " annotation_total=" << annotation_total
                << " annotation_failures=" << annotation_failures
                << " solve_us=" << stats.solve_time_microseconds << '\n';
    }
    return annotation_failures == 0 ? 0 : 3;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
