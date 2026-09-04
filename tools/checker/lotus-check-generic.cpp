#include "Checker/Framework/CheckerDriver.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/CheckerRegistry.h"
#include "Checker/Framework/CheckerSpecLoader.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"
#include "CheckerToolEntrypoints.h"

#include <array>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::OptionCategory GenericSelectionCategory(
    "Generic Checker Selection Options",
    "Options for selecting registry-backed declarative checkers");
static cl::OptionCategory GenericExecutionCategory(
    "Generic Checker Execution Options",
    "Options for running the generic declarative checker driver");
static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::value_desc("bitcode"), cl::init(""),
                  cl::cat(GenericExecutionCategory),
                  cl::sub(*cl::TopLevelSubCommand),
                  cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<bool>
    ListCheckers("list-checkers",
                 cl::desc("List available checker ids and exit"),
                 cl::cat(GenericSelectionCategory), cl::init(false),
                 cl::sub(*cl::TopLevelSubCommand),
                 cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<bool> ListParameters(
    "list-parameters",
    cl::desc("List global and engine-qualified parameters and exit"),
    cl::cat(GenericSelectionCategory), cl::init(false),
    cl::sub(*cl::TopLevelSubCommand));
static cl::opt<std::string> CategoryFilter(
    "generic.category", cl::desc("Run only checkers in the given category"),
    cl::value_desc("category"), cl::init(""), cl::cat(GenericSelectionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
static cl::opt<std::string> BuiltinSpecDir(
    "generic.spec-dir",
    cl::desc("Load declarative checker specs from this directory"),
    cl::value_desc("dir"), cl::init(""), cl::cat(GenericExecutionCategory),
    cl::sub(*cl::TopLevelSubCommand),
    cl::sub(lotus::checker::tooling::genericSubCommand()));
namespace {

struct EngineDescriptor {
  StringRef name;
  StringRef summary;
  lotus::checker::EngineKind kind;
  cl::SubCommand &(*subcommand)();
  int (*run)(const char *);
};

const std::array<EngineDescriptor, 9> &engineDescriptors() {
  static const std::array<EngineDescriptor, 9> descriptors = {{
      {"generic", "Registry-backed declarative checkers",
       lotus::checker::EngineKind::Declarative,
       lotus::checker::tooling::genericSubCommand, runGenericCheckerTool},
      {"ae", "Abstract execution", lotus::checker::EngineKind::AE,
       lotus::checker::tooling::aeSubCommand, runAECheckerTool},
      {"kint", "Integer bug detection", lotus::checker::EngineKind::KINT,
       lotus::checker::tooling::kintSubCommand, runKintCheckerTool},
      {"taint", "IFDS taint analysis", lotus::checker::EngineKind::Taint,
       lotus::checker::tooling::taintSubCommand, runTaintCheckerTool},
      {"concur", "Concurrency checking",
       lotus::checker::EngineKind::Concurrency,
       lotus::checker::tooling::concurrencySubCommand,
       runConcurrencyCheckerTool},
      {"pulse", "Pulse memory-safety analysis",
       lotus::checker::EngineKind::Pulse,
       lotus::checker::tooling::pulseSubCommand, runPulseCheckerTool},
      {"fitx", "FiTx typestate analysis", lotus::checker::EngineKind::FiTx,
       lotus::checker::tooling::fitxSubCommand, runFiTxCheckerTool},
      {"saber", "Sparse value-flow checking", lotus::checker::EngineKind::Saber,
       lotus::checker::tooling::saberSubCommand, runSaberCheckerTool},
      {"symex", "Symbolic execution", lotus::checker::EngineKind::SymExec,
       lotus::checker::tooling::symexSubCommand, runSymExCheckerTool},
  }};
  return descriptors;
}

const EngineDescriptor *findEngineDescriptor(StringRef name) {
  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    if (descriptor.name == name) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool isHelpFlag(StringRef arg) {
  return arg == "-h" || arg == "--help" || arg == "--help-hidden" ||
         arg == "--help-list" || arg == "--help-list-hidden";
}

bool isKnownCheckerSubcommand(StringRef arg) {
  return findEngineDescriptor(arg) != nullptr;
}

std::optional<std::string> requestedSubcommand(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    StringRef arg(argv[i]);
    if (arg == "--") {
      return std::nullopt;
    }
    if (!arg.empty() && arg[0] != '-') {
      if (isKnownCheckerSubcommand(arg)) {
        return arg.str();
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

bool helpRequested(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (StringRef(argv[i]) == "--") {
      break;
    }
    if (isHelpFlag(argv[i])) {
      return true;
    }
  }
  return false;
}

bool hiddenHelpRequested(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    StringRef argument(argv[index]);
    if (argument == "--") {
      break;
    }
    if (argument == "--help-hidden" || argument == "--help-list-hidden") {
      return true;
    }
  }
  return false;
}

cl::SubCommand *checkerSubcommand(StringRef engine) {
  const EngineDescriptor *descriptor = findEngineDescriptor(engine);
  return descriptor ? &descriptor->subcommand() : nullptr;
}

void printTopLevelHelp() {
  outs() << "OVERVIEW: Lotus checker front-end\n\n"
         << "USAGE:\n"
         << "  lotus-check --engine=<name> [engine options] <input bitcode>\n"
         << "  lotus-check --list-checkers\n"
         << "  lotus-check --list-parameters\n\n"
         << "ENGINES:\n";
  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    outs() << "  " << formatv("{0,-8}", descriptor.name) << descriptor.summary
           << "\n";
  }
  outs() << "\nPARAMETER NAMESPACES:\n"
         << "  Shared parameters use --<parameter>.\n"
         << "  Engine-specific parameters use --<engine>.<parameter>.\n\n"
         << "Use --list-parameters for all parameter descriptions, or\n"
         << "--engine=<name> --help for one engine.\n";
}

bool isParameterOption(const cl::Option &option) {
  if (option.isPositional() || option.getOptionHiddenFlag() != cl::NotHidden) {
    return false;
  }
  static const std::set<StringRef> metaOptions = {
      "help",    "help-hidden",    "help-list", "help-list-hidden",
      "version", "list-parameters"};
  return !metaOptions.count(option.ArgStr);
}

void printParameterOptions(const std::set<cl::Option *> &options) {
  std::vector<cl::Option *> sorted(options.begin(), options.end());
  llvm::sort(sorted, [](const cl::Option *left, const cl::Option *right) {
    return left->ArgStr < right->ArgStr;
  });
  size_t width = 0;
  for (const cl::Option *option : sorted) {
    width = std::max(width, option->getOptionWidth());
  }
  for (const cl::Option *option : sorted) {
    option->printOptionInfo(width);
  }
}

void printParameterDescriptions() {
  std::set<cl::Option *> globalOptions;
  bool firstEngine = true;
  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    std::set<cl::Option *> currentOptions;
    for (const auto &entry : descriptor.subcommand().OptionsMap) {
      if (entry.second && isParameterOption(*entry.second)) {
        currentOptions.insert(entry.second);
      }
    }
    if (firstEngine) {
      globalOptions = std::move(currentOptions);
      firstEngine = false;
      continue;
    }
    for (auto iterator = globalOptions.begin();
         iterator != globalOptions.end();) {
      if (!currentOptions.count(*iterator)) {
        iterator = globalOptions.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }

  outs() << "GLOBAL PARAMETERS\n"
         << "  Shared by checker engines and intentionally unqualified.\n\n";
  printParameterOptions(globalOptions);

  outs() << "\nTo set an engine parameter, use "
            "--<engine>.<parameter>=<value>.\n";
  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    const std::string prefix = (descriptor.name + ".").str();
    std::set<cl::Option *> engineOptions;
    for (const auto &entry : descriptor.subcommand().OptionsMap) {
      if (entry.second && isParameterOption(*entry.second) &&
          entry.second->ArgStr.startswith(prefix)) {
        engineOptions.insert(entry.second);
      }
    }
    if (engineOptions.empty()) {
      continue;
    }
    outs() << "\n[engine] " << descriptor.name << "\n\n";
    printParameterOptions(engineOptions);
  }
}

void printEngineHelp(StringRef engine, cl::SubCommand &subcommand,
                     bool showHidden) {
  outs() << "OVERVIEW: Lotus " << engine << " engine\n\n"
         << "USAGE: lotus-check --engine=" << engine
         << " [options] <input bitcode file>\n\n"
         << "OPTIONS:\n\n";

  std::set<cl::Option *> uniqueOptions;
  for (const auto &entry : subcommand.OptionsMap) {
    cl::Option *option = entry.second;
    if (!option || option->isPositional() ||
        (!showHidden && option->getOptionHiddenFlag() != cl::NotHidden)) {
      continue;
    }
    uniqueOptions.insert(option);
  }

  std::vector<cl::Option *> options(uniqueOptions.begin(), uniqueOptions.end());
  llvm::sort(options, [](const cl::Option *left, const cl::Option *right) {
    return left->ArgStr < right->ArgStr;
  });
  size_t width = 0;
  for (const cl::Option *option : options) {
    width = std::max(width, option->getOptionWidth());
  }
  for (const cl::Option *option : options) {
    option->printOptionInfo(width);
  }

  const EngineDescriptor *descriptor = findEngineDescriptor(engine);
  if (descriptor &&
      descriptor->kind != lotus::checker::EngineKind::Declarative) {
    ArrayRef<lotus::checker::NativeCheckDescriptor> checks =
        lotus::checker::getBuiltinNativeChecks(descriptor->kind);
    if (!checks.empty()) {
      outs() << "\nCHECKS:\n\n";
      for (const auto &check : checks) {
        outs() << "  " << formatv("{0,-28}", check.id) << check.title;
        if (check.default_enabled) {
          outs() << " (default)";
        }
        outs() << "\n";
      }
    }
  }
}

StringRef cliEngineName(lotus::checker::EngineKind engine) {
  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    if (descriptor.kind == engine) {
      return descriptor.name;
    }
  }
  llvm_unreachable("unhandled checker engine");
}

Expected<std::vector<std::string>> normalizeEngineSelectionArgs(int argc,
                                                                char **argv) {
  if (argc > 1 && isKnownCheckerSubcommand(argv[1])) {
    return createStringError(
        inconvertibleErrorCode(),
        "engine subcommands are not part of the public CLI; use --engine=%s",
        argv[1]);
  }

  std::optional<std::string> selectedEngine;
  std::vector<std::string> normalized;
  normalized.reserve(argc + 1);
  normalized.push_back(argv[0]);

  for (int index = 1; index < argc; ++index) {
    StringRef argument(argv[index]);
    if (argument == "--") {
      normalized.push_back(argv[index]);
      for (++index; index < argc; ++index) {
        normalized.push_back(argv[index]);
      }
      break;
    }
    StringRef engineName;
    if (argument.consume_front("--engine=")) {
      engineName = argument;
    } else if (argument == "--engine") {
      if (++index >= argc) {
        return createStringError(inconvertibleErrorCode(),
                                 "--engine requires a value");
      }
      engineName = argv[index];
    } else {
      normalized.push_back(argv[index]);
      continue;
    }

    if (selectedEngine) {
      return createStringError(inconvertibleErrorCode(),
                               "--engine may only be specified once");
    }
    if (!isKnownCheckerSubcommand(engineName)) {
      std::string expected;
      for (const EngineDescriptor &descriptor : engineDescriptors()) {
        if (!expected.empty()) {
          expected += ", ";
        }
        expected += descriptor.name.str();
      }
      return createStringError(inconvertibleErrorCode(),
                               "invalid engine '%s'; expected %s",
                               engineName.str().c_str(), expected.c_str());
    }
    selectedEngine = engineName.str();
  }

  if (selectedEngine) {
    normalized.insert(normalized.begin() + 1, *selectedEngine);
  }
  return normalized;
}

Error validateLongOptionSpellings(ArrayRef<std::string> arguments) {
  cl::SubCommand *subcommand = nullptr;
  if (arguments.size() > 1 && isKnownCheckerSubcommand(arguments[1])) {
    subcommand = checkerSubcommand(arguments[1]);
  } else {
    subcommand = &*cl::TopLevelSubCommand;
  }

  bool afterSentinel = false;
  for (size_t index = 1; index < arguments.size(); ++index) {
    StringRef argument(arguments[index]);
    if (argument == "--") {
      afterSentinel = true;
      continue;
    }
    if (afterSentinel || !argument.startswith("--")) {
      continue;
    }
    StringRef spelling = argument.drop_front(2).split('=').first;
    if (spelling.empty() || subcommand->OptionsMap.count(spelling)) {
      continue;
    }
    return createStringError(inconvertibleErrorCode(),
                             "Unknown command line argument '--%s'.",
                             spelling.str().c_str());
  }
  return Error::success();
}

Expected<lotus::checker::CheckerRegistry> buildRegistry() {
  lotus::checker::CheckerRegistry registry;
  if (auto error = lotus::checker::registerBuiltinNativeCheckers(registry)) {
    return std::move(error);
  }

  std::string specDir;
  if (BuiltinSpecDir.getNumOccurrences() != 0) {
    specDir = BuiltinSpecDir;
  } else if (auto environment =
                 sys::Process::GetEnv("LOTUS_CHECKER_SPEC_DIR")) {
    specDir = *environment;
  } else if (sys::fs::is_directory(LOTUS_INSTALL_CHECKER_SPEC_DIR)) {
    specDir = LOTUS_INSTALL_CHECKER_SPEC_DIR;
  } else {
    specDir = LOTUS_SOURCE_CHECKER_SPEC_DIR;
  }

  lotus::checker::CheckerSpecLoader loader;
  auto specs_or = loader.loadFromDirectory(specDir);
  if (!specs_or) {
    return specs_or.takeError();
  }
  for (const auto &spec : *specs_or) {
    if (auto error = registry.registerDeclarative(spec)) {
      return std::move(error);
    }
  }

  return registry;
}

} // namespace

int runGenericCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto registry_or = buildRegistry();
  if (!registry_or) {
    logAllUnhandledErrors(registry_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  lotus::checker::CheckerRegistry registry = std::move(*registry_or);

  if (ListCheckers) {
    outs() << "ID\tENGINE\tMODE\tDEFAULT\tCATEGORY\tTITLE\n";
    for (const auto *descriptor : registry.list()) {
      if (descriptor->isDeclarative()) {
        outs() << descriptor->metadata.id << "\tgeneric\tgeneric\t"
               << (descriptor->metadata.default_enabled ? "yes" : "no") << "\t"
               << descriptor->metadata.category << "\t"
               << descriptor->metadata.title << "\n";
        continue;
      }
      for (const auto &check : lotus::checker::getBuiltinNativeChecks(
               descriptor->metadata.engine)) {
        outs() << check.id << "\t" << cliEngineName(descriptor->metadata.engine)
               << "\tnative\t" << (check.default_enabled ? "yes" : "no") << "\t"
               << descriptor->metadata.category << "\t" << check.title << "\n";
      }
    }
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  if (InputFilename.empty()) {
    errs() << "error: input bitcode file is required unless --list-checkers is "
              "used\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  LLVMContext context;
  SMDiagnostic error;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("generic", *module, mgr);

  std::vector<const lotus::checker::CheckerDescriptor *> selection;
  auto requestedOr = lotus::checker::tooling::parseCheckSelection("generic");
  if (!requestedOr) {
    logAllUnhandledErrors(requestedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const bool selectAll = lotus::checker::tooling::hasExplicitCheckSelection() &&
                         *requestedOr == std::vector<std::string>{"all"};
  if (lotus::checker::tooling::hasExplicitCheckSelection() && !selectAll) {
    for (const std::string &id : *requestedOr) {
      auto descriptor_or = registry.findById(id);
      if (!descriptor_or) {
        logAllUnhandledErrors(descriptor_or.takeError(), errs(), "");
        return lotus::checker::tooling::EXIT_ERROR;
      }
      const auto *descriptor = *descriptor_or;
      if (!descriptor->isDeclarative()) {
        errs() << "error: checker engine '" << descriptor->metadata.id
               << "' cannot run in generic mode\n"
               << "hint: use --engine="
               << cliEngineName(descriptor->metadata.engine)
               << " --checks=<id>\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
      if (!CategoryFilter.empty() &&
          descriptor->metadata.category != CategoryFilter) {
        continue;
      }
      selection.push_back(descriptor);
    }
  } else {
    selection = registry.select(CategoryFilter,
                                lotus::checker::EngineKind::Declarative);
    if (!lotus::checker::tooling::hasExplicitCheckSelection() &&
        CategoryFilter.empty()) {
      std::vector<const lotus::checker::CheckerDescriptor *> defaults;
      for (const auto *descriptor : selection) {
        if (descriptor->metadata.default_enabled) {
          defaults.push_back(descriptor);
        }
      }
      selection = std::move(defaults);
    }
  }

  if (selection.empty()) {
    errs() << "error: no checkers selected\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  lotus::checker::CheckerContext checker_context{*module};
  lotus::checker::CheckerDriver driver(registry, checker_context);
  auto diagnostics_or = driver.run(selection);
  if (!diagnostics_or) {
    logAllUnhandledErrors(diagnostics_or.takeError(), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (auto report_error = driver.emitToReportManager(*diagnostics_or)) {
    logAllUnhandledErrors(std::move(report_error), errs(), "");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  stats.emit();
  return lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
}

int main(int argc, char **argv) {
  auto normalizedArgsOr = normalizeEngineSelectionArgs(argc, argv);
  if (!normalizedArgsOr) {
    logAllUnhandledErrors(normalizedArgsOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  std::vector<std::string> normalizedArgs = std::move(*normalizedArgsOr);
  std::vector<char *> normalizedArgv;
  normalizedArgv.reserve(normalizedArgs.size() + 1);
  for (std::string &argument : normalizedArgs) {
    normalizedArgv.push_back(argument.data());
  }
  normalizedArgv.push_back(nullptr);
  int normalizedArgc = static_cast<int>(normalizedArgs.size());
  char **argumentVector = normalizedArgv.data();

  sys::PrintStackTraceOnErrorSignal(argumentVector[0]);
  PrettyStackTraceProgram stack_trace(normalizedArgc, argumentVector);
  llvm::InitLLVM init_llvm(normalizedArgc, argumentVector);
  llvm_shutdown_obj shutdown;
  report_options::initializeReportOptions();

  const bool wants_help = helpRequested(normalizedArgc, argumentVector);
  const auto requested_subcommand =
      requestedSubcommand(normalizedArgc, argumentVector);
  if (wants_help) {
    if (!requested_subcommand) {
      printTopLevelHelp();
      return lotus::checker::tooling::EXIT_SUCCESS_CODE;
    }
    cl::SubCommand *subcommand = checkerSubcommand(*requested_subcommand);
    assert(subcommand && "normalized engine must have a subcommand");
    const bool showHidden = hiddenHelpRequested(normalizedArgc, argumentVector);
    printEngineHelp(*requested_subcommand, *subcommand, showHidden);
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  if (Error spellingError = validateLongOptionSpellings(normalizedArgs)) {
    logAllUnhandledErrors(std::move(spellingError), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (!cl::ParseCommandLineOptions(
          normalizedArgc, argumentVector,
          "Lotus checker front-end\n"
          "  Select one engine with --engine=<name>.\n"
          "  Example: 'lotus-check --engine=symex --help' shows Symbolic "
          "Execution options.\n",
          &errs())) {
    return lotus::checker::tooling::EXIT_ERROR;
  }

  if (!lotus::checker::tooling::validateReportOptions()) {
    return lotus::checker::tooling::EXIT_ERROR;
  }
  if (ListParameters) {
    printParameterDescriptions();
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }
  lotus::checker::tooling::configureCommonLogging();

  for (const EngineDescriptor &descriptor : engineDescriptors()) {
    if (descriptor.subcommand()) {
      return descriptor.run(argumentVector[0]);
    }
  }
  if (ListCheckers) {
    return runGenericCheckerTool(argumentVector[0]);
  }

  errs() << "error: no engine selected\n";
  errs() << "hint: use --engine=<name>, --list-checkers, or "
            "--list-parameters\n";
  return lotus::checker::tooling::EXIT_ERROR;
}
