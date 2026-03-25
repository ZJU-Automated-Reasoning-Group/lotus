#include "Verification/Transform/RenameVerifierFuns.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

using namespace llvm;

static cl::opt<std::string> source_name("rename-verifier-funs-source",
                                        cl::desc("Specify source filename"),
                                        cl::value_desc("filename"));

namespace lotus {
namespace verification {
namespace transform {

char RenameVerifierFunsPass::ID = 0;

static std::string getName(const std::string &line) {
  std::istringstream iss(line);
  std::string sub, var;
  while (iss >> sub) {
    if (sub == "=") {
      break;
    }
    var = std::move(sub);
  }

  if (!var.empty() && sub == "=") {
    // Check also that after = follows the __VERIFIER_* call
    iss >> sub;
    if (sub.compare(0, 18, "__VERIFIER_nondet_") == 0)
      return var;
  }

  return "--";
}

static void replaceCall(Module &M, CallInst *CI, unsigned line,
                        const std::string &var) {
  std::string parent_name =
      cast<Function>(CI->getParent()->getParent())->getName().str();
  std::string name = parent_name + ":" + var + ":" + std::to_string(line);
  Function *called_func = CI->getCalledFunction();
  auto new_func = M.getOrInsertFunction(
      called_func->getName().str() + "_named", called_func->getAttributes(),
      called_func->getReturnType(), Type::getInt8PtrTy(M.getContext()));

  std::vector<Value *> args;
  Constant *name_const = ConstantDataArray::getString(M.getContext(), name);
  GlobalVariable *nameG =
      new GlobalVariable(M, name_const->getType(), true /*constant*/,
                         GlobalVariable::PrivateLinkage, name_const);
  args.push_back(
      ConstantExpr::getPointerCast(nameG, Type::getInt8PtrTy(M.getContext())));

  CallInst *new_CI = CallInst::Create(new_func, args);
  SmallVector<std::pair<unsigned, MDNode *>, 8> metadata;
  CI->getAllMetadata(metadata);
  // Copy the metadata
  for (auto &md : metadata)
    new_CI->setMetadata(md.first, md.second);
  // Copy the attributes
  new_CI->setAttributes(CI->getAttributes());

  new_CI->insertBefore(CI);
  CI->replaceAllUsesWith(new_CI);
  CI->eraseFromParent();
}

bool RenameVerifierFunsPass::runOnModule(Module &M) {
  if (source_name.empty()) {
    // Skip if no source file specified
    return false;
  }

  // Collect calls to replace
  std::vector<std::pair<unsigned, CallInst *>> calls_to_replace;
  std::set<unsigned> lines_nums;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    StringRef name = F.getName();
    if (!name.startswith("__VERIFIER_nondet_"))
      continue;

    for (auto use_it = F.use_begin(), use_end = F.use_end(); use_it != use_end;
         ++use_it) {
      CallInst *CI = dyn_cast<CallInst>(use_it->getUser());
      if (CI) {
        const DebugLoc &Loc = CI->getDebugLoc();
        if (Loc) {
          calls_to_replace.emplace_back(Loc.getLine(), CI);
          lines_nums.insert(Loc.getLine());
        }
      }
    }
  }

  if (calls_to_replace.empty())
    return false;

  // Map source lines
  std::map<unsigned, std::string> lines;
  std::ifstream file(source_name);
  if (!file.is_open()) {
    errs() << "Couldn't open file: " << source_name << "\n";
    return false;
  }

  unsigned n = 1;
  std::string line;
  while (getline(file, line)) {
    if (lines_nums.count(n) > 0)
      lines[n] = std::move(line);
    ++n;
  }
  file.close();

  // Replace calls
  for (auto &pr : calls_to_replace) {
    unsigned line_num = pr.first;
    CallInst *CI = pr.second;

    if (lines.find(line_num) == lines.end())
      continue;

    std::string line = lines[line_num];
    replaceCall(M, CI, line_num, getName(line));
  }

  return !calls_to_replace.empty();
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::RenameVerifierFunsPass>
    X("rename-verifier-funs",
      "Replace calls to verifier functions with calls to named versions");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createRenameVerifierFunsPass() {
  return new RenameVerifierFunsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
