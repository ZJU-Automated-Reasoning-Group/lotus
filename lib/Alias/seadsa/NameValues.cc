#include "Alias/seadsa/support/NameValues.hh"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <string>

using namespace llvm;

namespace seadsa {

char NameValues::ID = 0;

bool NameValues::runOnModule(Module &M) {
  bool change = false;
  for (Module::iterator FI = M.begin(), E = M.end(); FI != E; ++FI)
    change |= runOnFunction(*FI);
  return change;
}

// Helper function to tokenize a string
static std::vector<std::string> tokenize(const std::string &str, const std::string &delimiters) {
  std::vector<std::string> tokens;
  size_t start = 0;
  size_t end = str.find_first_of(delimiters);
  
  while (end != std::string::npos) {
    if (end != start) {
      tokens.push_back(str.substr(start, end - start));
    }
    start = end + 1;
    end = str.find_first_of(delimiters, start);
  }
  
  if (start < str.length()) {
    tokens.push_back(str.substr(start));
  }
  
  return tokens;
}

bool NameValues::runOnFunction(Function &F) {
  bool change = false;

  // -- print to string
  std::string funcAsm;
  raw_string_ostream out(funcAsm);
  out << F;
  out.flush();

  // Tokenize by newlines
  std::vector<std::string> lines;
  std::istringstream stream(funcAsm);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  
  auto line_iter = lines.begin();

  // -- skip function attributes
  if (line_iter != lines.end() && line_iter->find("; Function Attrs:") == 0)
    ++line_iter;

  unsigned ArgIdx = 0;
  for (Argument &arg : F.args()) {
    ++ArgIdx;
    if (!arg.hasName())
      arg.setName("_arg" + std::to_string(ArgIdx));
  }

  // -- skip function definition line
  if (line_iter != lines.end())
    ++line_iter;

  for (Function::iterator BI = F.begin(), BE = F.end();
       BI != BE && line_iter != lines.end(); ++BI) {
    BasicBlock &BB = *BI;

    if (!BB.hasName()) {
      std::string bb_line = *line_iter;
      auto names = tokenize(bb_line, " :\t%@");
      std::string bb_name = names.empty() ? "bb" : names[0];
      if (bb_name == ";")
        bb_name = "bb";
      BB.setName("_" + bb_name);
      change = true;
    }
    ++line_iter;

    for (BasicBlock::iterator II = BB.begin(), IE = BB.end();
         II != IE && line_iter != lines.end(); ++II) {
      Instruction &I = *II;
      if (!I.hasName() && !(I.getType()->isVoidTy())) {
        std::string inst_line = *line_iter;
        auto names = tokenize(inst_line, " :\t%@");
        std::string inst_name = names.empty() ? "tmp" : names[0];
        I.setName("_" + inst_name);
        change = true;
      }
      ++line_iter;
    }
  }
  return change;
}
} // namespace seadsa

static llvm::RegisterPass<seadsa::NameValues> X("seadsa-name-values",
                                                 "Names all unnamed values");
