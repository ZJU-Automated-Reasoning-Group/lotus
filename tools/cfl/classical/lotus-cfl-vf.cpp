#include "CFL/Classical/Clients/ValueFlow/ValueFlowClient.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

using namespace lotus::analysis;
using namespace lotus::cfl::classical;

namespace {

struct Options {
  std::string input;
  SolverBackend backend = SolverBackend::SparseSet;
  bool prepare_svfg = true;
  bool json_stats = false;
  std::string dump_svfg;
  std::string query_source;
  std::string query_target;
};

void usage(std::ostream &stream) {
  stream << "Usage: lotus-cfl-vf [options] INPUT.{ll,bc}\n"
            "Options:\n"
            "  --solver sparse-set|sparse-bitvector|transitive-closure\n"
            "  --query SOURCE,TARGET       Query named LLVM values\n"
            "                              (use FUNCTION::VALUE for locals)\n"
            "  --dump-svfg FILE            Write the prepared SVFG as DOT\n"
            "  --no-prepare                Keep dereference and strong-update "
            "edges\n"
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
      if (selected == "sparse-set") {
        options.backend = SolverBackend::SparseSet;
      } else if (selected == "sparse-bitvector") {
        options.backend = SolverBackend::SparseBitVector;
      } else if (selected == "transitive-closure") {
        options.backend = SolverBackend::TransitiveClosure;
      } else {
        throw std::invalid_argument("Unknown solver: " + selected);
      }
    } else if (argument == "--query") {
      const std::string query = value();
      const auto comma = query.find(',');
      if (comma == std::string::npos || comma == 0 ||
          comma + 1 == query.size()) {
        throw std::invalid_argument("Value-flow query must be SOURCE,TARGET");
      }
      options.query_source = query.substr(0, comma);
      options.query_target = query.substr(comma + 1);
    } else if (argument == "--dump-svfg") {
      options.dump_svfg = value();
    } else if (argument == "--no-prepare") {
      options.prepare_svfg = false;
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

const llvm::Value *findLocalValue(const llvm::Function &function,
                                  const std::string &name) {
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
  return nullptr;
}

const llvm::Value *findNamedValue(const llvm::Module &module,
                                  const std::string &qualified_name) {
  const auto separator = qualified_name.find("::");
  if (separator != std::string::npos) {
    const std::string function_name = qualified_name.substr(0, separator);
    const std::string value_name = qualified_name.substr(separator + 2);
    const llvm::Function *function = module.getFunction(function_name);
    return function ? findLocalValue(*function, value_name) : nullptr;
  }

  if (const auto *global = module.getNamedGlobal(qualified_name)) {
    return global;
  }
  if (const auto *function = module.getFunction(qualified_name)) {
    return function;
  }

  const llvm::Value *match = nullptr;
  for (const llvm::Function &function : module) {
    const llvm::Value *candidate = findLocalValue(function, qualified_name);
    if (!candidate) {
      continue;
    }
    if (match) {
      throw std::invalid_argument("Ambiguous LLVM value name '" +
                                  qualified_name +
                                  "'; qualify it as FUNCTION::VALUE");
    }
    match = candidate;
  }
  return match;
}

const SVFGNode *findSVFGNode(const llvm::Module &module, const SVFG &svfg,
                             const std::string &name) {
  const llvm::Value *value = findNamedValue(module, name);
  if (!value) {
    throw std::invalid_argument("Unknown LLVM value: " + name);
  }
  const SVFGNode *node = svfg.getValueNode(value);
  if (!node) {
    throw std::invalid_argument("LLVM value has no SVFG node: " + name);
  }
  return node;
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
      return 1;
    }

    ICFG icfg;
    ICFGBuilder icfg_builder(&icfg);
    icfg_builder.build(module.get());

    SVFGBuilderConfig builder_options;
    builder_options.usePointerAnalysis = true;
    builder_options.buildMSSA = true;
    builder_options.resolveIndirectCalls = true;
    SVFGBuilder svfg_builder(builder_options);
    std::unique_ptr<SVFG> svfg(svfg_builder.build(&icfg));

    SVFGPreparationStatistics preparation;
    if (options.prepare_svfg) {
      preparation = prepareSVFGForCFL(*svfg);
    }
    if (!options.dump_svfg.empty()) {
      svfg->dump(options.dump_svfg);
    }

    ValueFlowClient client = ValueFlowClient::fromSVFG(*svfg);
    const ReachabilityStats statistics = client.solve(options.backend);

    if (!options.query_source.empty()) {
      const SVFGNode *source =
          findSVFGNode(*module, *svfg, options.query_source);
      const SVFGNode *target =
          findSVFGNode(*module, *svfg, options.query_target);
      std::cout << "flow="
                << (client.hasFlow(source->getId(), target->getId()) ? "yes"
                                                                     : "no")
                << " source=" << options.query_source
                << " source_node=" << source->getId()
                << " target=" << options.query_target
                << " target_node=" << target->getId() << '\n';
    }

    if (options.json_stats) {
      std::cout << "{\"solver\":\"" << solverBackendName(options.backend)
                << "\",\"svfg_nodes\":" << client.graph().vertexCount()
                << ",\"cfl_nodes\":" << client.graph().vertexCount()
                << ",\"input_edges\":" << statistics.input_edges
                << ",\"derived_edges\":" << statistics.added_edges
                << ",\"relation_edges\":" << statistics.relation_edges
                << ",\"start_edges\":" << statistics.start_symbol_edges
                << ",\"processed_items\":" << statistics.processed_work_items
                << ",\"dereference_edges_removed\":"
                << preparation.dereference_edges_removed
                << ",\"strong_update_stores\":"
                << preparation.strong_update_stores
                << ",\"strong_update_edges_removed\":"
                << preparation.strong_update_edges_removed << "}\n";
    } else {
      std::cout << "solver=" << solverBackendName(options.backend)
                << " svfg_nodes=" << client.graph().vertexCount()
                << " cfl_nodes=" << client.graph().vertexCount()
                << " input_edges=" << statistics.input_edges
                << " derived_edges=" << statistics.added_edges
                << " relation_edges=" << statistics.relation_edges
                << " start_edges=" << statistics.start_symbol_edges
                << " dereference_edges_removed="
                << preparation.dereference_edges_removed
                << " strong_update_edges_removed="
                << preparation.strong_update_edges_removed << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
