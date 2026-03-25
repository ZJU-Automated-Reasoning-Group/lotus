/*
 * CLI utilities for alias analysis tools
 * 
 * Implementation of shared functionality for loading IR modules and
 * managing pointer analysis configuration files for command-line tools.
 */

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"

#include "Alias/Spec/AliasSpecManager.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace alias {
namespace tools {

// Command-line options for pointer analysis configuration files
llvm::cl::opt<std::string> ConfigFile(
    "config",
    llvm::cl::desc("Path to config spec file (e.g., ptr.spec). Can be specified multiple times or use comma-separated paths"),
    llvm::cl::value_desc("filepath"));

llvm::cl::list<std::string> ConfigFiles(
    "config-file",
    llvm::cl::desc("Path to config spec file (alternative to -config)"),
    llvm::cl::value_desc("filepath"));

std::unique_ptr<llvm::Module> loadIRModule(
    const std::string &Filename,
    llvm::LLVMContext &Context,
    llvm::SMDiagnostic &Err,
    const char *ProgramName) {
    
    auto Module = llvm::parseIRFile(Filename, Err, Context);
    
    if (!Module) {
        if (ProgramName) {
            Err.print(ProgramName, llvm::errs());
        } else {
            Err.print("program", llvm::errs());
        }
    }
    
    return Module;
}

std::vector<std::string> collectConfigFilePaths() {
    std::vector<std::string> specFilePaths;
    
    // Collect config files from -config option (comma-separated)
    if (!ConfigFile.empty()) {
        std::string config = ConfigFile.getValue();
        size_t pos = 0;
        while ((pos = config.find(',')) != std::string::npos) {
            std::string path = config.substr(0, pos);
            if (!path.empty()) {
                specFilePaths.push_back(path);
            }
            config.erase(0, pos + 1);
        }
        if (!config.empty()) {
            specFilePaths.push_back(config);
        }
    }
    
    // Add files from -config-file option
    for (const auto &path : ConfigFiles) {
        specFilePaths.push_back(path);
    }
    
    return specFilePaths;
}

std::unique_ptr<::lotus::alias::AliasSpecManager> createAliasSpecManager(
    const std::vector<std::string> &SpecFilePaths,
    llvm::Module *Module) {
    
    std::unique_ptr<::lotus::alias::AliasSpecManager> specManager;
    
    if (!SpecFilePaths.empty()) {
        specManager = std::make_unique<::lotus::alias::AliasSpecManager>(SpecFilePaths);
    } else {
        specManager = std::make_unique<::lotus::alias::AliasSpecManager>();
    }
    
    // Initialize with module for better name matching if provided
    if (Module) {
        specManager->initialize(*Module);
    }
    
    return specManager;
}

void printLoadedConfigFiles(const ::lotus::alias::AliasSpecManager &SpecManager) {
    const auto &loadedFiles = SpecManager.getLoadedSpecFiles();
    if (!loadedFiles.empty()) {
        llvm::errs() << "Config files: ";
        for (size_t i = 0; i < loadedFiles.size(); ++i) {
            if (i > 0) llvm::errs() << ", ";
            llvm::errs() << loadedFiles[i];
        }
        llvm::errs() << "\n";
    } else {
        llvm::errs() << "Config files: (none loaded)\n";
    }
}

} // namespace tools
} // namespace alias
} // namespace lotus
