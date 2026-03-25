// SymAbsAI static analysis tool
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/Checks.h"
#include "Verification/SymAbsAI/Core/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/FragmentDecomposition.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"
#include "Verification/SymAbsAI/Core/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Reporting.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace symabs_ai;
static void listConfigurationFiles() {
  const char *configDirs[] = {"../config/symabs-ai", "../../config/symabs-ai",
                              "../../../config/symabs-ai",
                              "./config/symabs-ai"};

  outs() << "Available configuration files:\n";
  bool foundAny = false;
  for (const char *dir : configDirs) {
    std::error_code EC;
    sys::fs::directory_iterator DirIt(dir, EC), DirEnd;
    if (EC)
      continue;

    SmallVector<std::string, 32> configs;
    for (; DirIt != DirEnd && !EC; DirIt.increment(EC)) {
      StringRef path = DirIt->path();
      if (path.endswith(".conf")) {
        configs.push_back(path.str());
        foundAny = true;
      }
    }
    if (!configs.empty()) {
      std::sort(configs.begin(), configs.end());
      for (const auto &cfg : configs)
        outs() << "  " << cfg << "\n";
    }
  }
  if (!foundAny) {
    outs() << "No configuration files found in config/symabs-ai/\n";
  }
  outs() << "\nSee config/symabs-ai/README.md for details.\n";
}

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::value_desc("bitcode"));

static cl::opt<std::string> ConfigFile(
    "config",
    cl::desc("Configuration file (see config/symabs-ai/ for examples)"),
    cl::value_desc("file"));

static cl::opt<std::string> FunctionName(
    "function",
    cl::desc("Function to analyze (default: main or first function)"),
    cl::value_desc("name"));

static cl::opt<std::string> AbstractDomainName(
    "abstract-domain",
    cl::desc("Abstract domain (use --list-domains for available options)"),
    cl::value_desc("domain"));

static cl::opt<bool> Verbose("verbose", cl::desc("Enable verbose output"),
                             cl::init(false));

static cl::opt<bool> ListFunctions("list-functions",
                                   cl::desc("List all functions in the module"),
                                   cl::init(false));

static cl::opt<bool>
    ListDomains("list-domains", cl::desc("List all available abstract domains"),
                cl::init(false));

static cl::opt<bool> ListConfigs("list-configs",
                                 cl::desc("List available configuration files"),
                                 cl::init(false));

static cl::opt<bool>
    ShowAllBlocks("show-all-blocks",
                  cl::desc("Show analysis results for all basic blocks"),
                  cl::init(false));

static cl::opt<bool>
    ShowExitBlocks("show-exit-blocks",
                   cl::desc("Show analysis results at exit blocks (return "
                            "statements) [default: enabled]"),
                   cl::init(false));

static cl::opt<std::string> FragmentStrategy(
    "fragment-strategy",
    cl::desc("Fragment strategy (Edges|Function|Headers|Body|Backedges)"),
    cl::value_desc("strategy"));

static cl::opt<std::string> MemoryModelVariant(
    "memory-model",
    cl::desc("Memory model (NoMemory|BlockModel|Aligned|LittleEndian)"),
    cl::value_desc("variant"));

static cl::opt<int> WideningDelay("widening-delay",
                                  cl::desc("Iterations before widening"),
                                  cl::init(-1));

static cl::opt<int> WideningFrequency("widening-frequency",
                                      cl::desc("Widen every N iterations"),
                                      cl::init(-1));

static cl::opt<bool>
    CheckAssertions("check-assertions",
                    cl::desc("Check for possibly violated assertions"),
                    cl::init(false));

static cl::opt<bool> CheckMemSafety(
    "check-memsafety",
    cl::desc("Check for possibly invalid memory accesses (requires RTTI)"),
    cl::init(false));

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(
      argc, argv,
      "SymAbsAI Static Analyzer - Abstract Interpretation for LLVM IR\n");
  VerboseEnable = Verbose;

  if (ListConfigs) {
    listConfigurationFiles();
    return 0;
  }

  if (ListDomains) {
    const auto &domains = DomainConstructor::all();
    if (domains.empty()) {
      outs() << "No abstract domains registered.\n";
      return 0;
    }
    outs() << "Available abstract domains:\n";
    for (const auto &domain : domains) {
      outs() << "  " << domain.name();
      if (!domain.description().empty())
        outs() << " - " << domain.description();
      outs() << "\n";
    }
    return 0;
  }

  if (InputFilename.empty()) {
    errs() << "Error: input bitcode file required.\n";
    return 1;
  }

  LLVMContext context;
  SMDiagnostic err;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }

  if (ListFunctions) {
    outs() << "Functions in module:\n";
    for (auto &F : *module)
      if (!F.isDeclaration())
        outs() << "  " << F.getName() << "\n";
    return 0;
  }

  Function *targetFunc = nullptr;
  if (FunctionName.empty()) {
    targetFunc = module->getFunction("main");
    if (!targetFunc)
      for (auto &F : *module)
        if (!F.isDeclaration()) {
          targetFunc = &F;
          break;
        }
  } else {
    targetFunc = module->getFunction(FunctionName);
  }

  if (!targetFunc) {
    errs() << "Error: Function '"
           << (FunctionName.empty() ? "main" : FunctionName.getValue())
           << "' not found\n";
    return 1;
  }

  try {
    configparser::Config config =
        ConfigFile.empty() ? configparser::Config()
                           : configparser::Config(ConfigFile.getValue());

    if (!FragmentStrategy.getValue().empty())
      config.set("FragmentDecomposition", "Strategy",
                 FragmentStrategy.getValue());
    if (!MemoryModelVariant.getValue().empty())
      config.set("MemoryModel", "Variant", MemoryModelVariant.getValue());
    if (WideningDelay >= 0)
      config.set("Analyzer", "WideningDelay", WideningDelay.getValue());
    if (WideningFrequency >= 0)
      config.set("Analyzer", "WideningFrequency", WideningFrequency.getValue());

    const auto &allDomains = DomainConstructor::all();
    DomainConstructor domain;
    std::string domainSource;
    bool fallbackToFirst = false;

    if (!AbstractDomainName.getValue().empty()) {
      auto it = std::find_if(allDomains.begin(), allDomains.end(),
                             [&](const auto &d) {
                               return d.name() == AbstractDomainName.getValue();
                             });
      if (it != allDomains.end()) {
        domain = *it;
        domainSource = "command line";
      }
    } else {
      DomainConstructor configDomain = config.get<DomainConstructor>(
          "AbstractDomain", "Variant", DomainConstructor());
      if (!configDomain.isInvalid() || allDomains.empty()) {
        domain = configDomain;
        domainSource =
            ConfigFile.empty() && !std::getenv("SYMBOLIC_ABSTRACTION_CONFIG")
                ? "built-in defaults"
                : "config";
      } else if (!allDomains.empty()) {
        domain = allDomains.front();
        fallbackToFirst = true;
        domainSource = "first registered";
      }
    }

    if (domain.isInvalid()) {
      if (AbstractDomainName.empty())
        errs() << "Error: no abstract domains registered.\n";
      else
        errs() << "Error: unknown domain '" << AbstractDomainName.getValue()
               << "'. Use --list-domains.\n";
      return 1;
    }

    std::string configSource;
    if (!ConfigFile.empty()) {
      configSource = ConfigFile.getValue();
    } else if (const char *env = std::getenv("SYMBOLIC_ABSTRACTION_CONFIG")) {
      configSource = std::string(env) + " (SYMBOLIC_ABSTRACTION_CONFIG)";
    } else {
      configSource = "<built-in defaults>";
    }

    const bool usingBuiltInDefaults = configSource == "<built-in defaults>";

    auto classifyOrigin = [&](bool setViaCLI) -> std::string {
      if (setViaCLI)
        return "command line";
      if (usingBuiltInDefaults)
        return "default";
      return "config";
    };

    std::string fragmentStrategyValue = config.get<std::string>(
        "FragmentDecomposition", "Strategy", "Function");
    std::string fragmentOrigin =
        classifyOrigin(!FragmentStrategy.getValue().empty());

    std::string analyzerVariant =
        config.get<std::string>("Analyzer", "Variant", "UnilateralAnalyzer");
    bool incremental = config.get<bool>("Analyzer", "Incremental", true);
    int wideningDelay = config.get<int>("Analyzer", "WideningDelay", 1);
    int wideningFrequency = config.get<int>("Analyzer", "WideningFrequency", 1);
    std::string wideningOrigin =
        classifyOrigin(WideningDelay >= 0 || WideningFrequency >= 0);

    std::string memoryVariant =
        config.get<std::string>("MemoryModel", "Variant", "NoMemory");
    int addressBits = config.get<int>("MemoryModel", "AddressBits", -1);
    std::string memoryOrigin =
        classifyOrigin(!MemoryModelVariant.getValue().empty());

    printEffectiveConfiguration(
        configSource, domain.name(), domainSource, fallbackToFirst,
        fragmentStrategyValue, fragmentOrigin, analyzerVariant, incremental,
        wideningDelay, wideningFrequency, wideningOrigin, memoryVariant,
        addressBits, memoryOrigin);

    outs() << "Analyzing function: " << targetFunc->getName() << "\n";

    if (!isInSSAForm(targetFunc)) {
      errs() << "Warning: Not in SSA form. Run mem2reg pass first.\n";
    }

    ModuleContext mctx(module.get(), config);
    FunctionContext fctx(targetFunc, &mctx);
    auto fragments = FragmentDecomposition::For(fctx);
    auto analyzer = Analyzer::New(fctx, fragments, domain);

    // Check assertions if requested
    if (CheckAssertions) {
      return runAssertionCheck(analyzer.get(), targetFunc);
    }

    // Check memory safety if requested
    // NOTE: This feature requires RTTI to be enabled (dynamic_cast)
    // Currently disabled as RTTI is not enabled in this build
    if (CheckMemSafety) {
      return runMemSafetyCheck(analyzer.get(), targetFunc);
    }

    // Show entry point results
    printEntryResult(analyzer.get(), targetFunc);

    // Show results for all blocks if requested
    if (ShowAllBlocks) {
      printAllBlocksResults(analyzer.get(), targetFunc);
    }
    // Show exit block results by default (triggers actual analysis),
    // or if explicitly requested
    else {
      printExitBlocksResults(analyzer.get(), targetFunc);
    }

    outs() << "Analysis completed successfully.\n";

  } catch (const std::exception &e) {
    errs() << "Error during analysis: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
