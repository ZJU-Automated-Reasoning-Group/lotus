#include "Alias/Dynamic/MemoryInstrument.h"

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>

using namespace llvm;

cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input file>"),
                                   cl::init("-"));
cl::opt<std::string> OutputFilename("o", cl::desc("Specify output filename"),
                                    cl::value_desc("filename"), cl::Required);

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseIRFile(InputFilename, error, context);
  if (!module) {
    error.print(InputFilename.data(), errs());
    return -1;
  }

  dynamic::MemoryInstrument().runOnModule(*module);

  std::error_code ec;
  ToolOutputFile outFile(OutputFilename, ec, sys::fs::OF_None);
  outFile.keep();
  WriteBitcodeToFile(*module, outFile.os());
}