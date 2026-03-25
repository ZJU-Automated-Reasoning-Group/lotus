#include "Solvers/SMT/SLOT/SMTFormula.h"

#include "Solvers/SMT/SLOT/SLOTExceptions.h"

#include <regex>

#ifndef LLMAPPING
#define LLMAPPING std::map<std::string, Value *>
#endif

namespace SLOT {
static context c;

SMTFormula::SMTFormula(LLVMContext &t_lcx, Module *t_lmodule,
                       IRBuilder<> &t_builder, std::string t_string,
                       std::string t_func_name)
    : lcx(t_lcx), lmodule(t_lmodule), builder(t_builder), string(t_string),
      func_name(t_func_name), contents(c) {
  // Parse the SMT-LIB input eagerly so we can predeclare LLVM arguments with
  // the same shape as the SMT variables. We keep one static Z3 context to
  // avoid recreating sorts on every translation.
  // Regular expression matching to get variables
  const std::string &s = string;
  std::smatch m;
  static const std::regex e(
      R"(\((declare-fun\s(\|.*\||[\~\!\@\$\%\^\&\*_\-\+\=\<\>\.\?\/A-Za-z0-9]+)\s*\(\s*\)\s*(\(\s*_\s*FloatingPoint\s*(\d+)\s*(\d+)\s*\)|Float16|Float32|Float64|Float128|FPN|Bool|\(\s*_\s*BitVec\s*(\d+)\s*\))\s*|declare-const\s(\|.*\||[\~\!\@\$\%\^\&\*_\-\+\=\<\>\.\?\/A-Za-z0-9]+)\s*(\(\s*_\s*FloatingPoint\s*(\d+)\s*(\d+)\s*\)|Float16|Float32|Float64|Float128|FPN|Bool|\(\s*_\s*BitVec\s*(\d+)\s*\))\s*)\))");
  std::string::const_iterator search_start = s.cbegin();

  std::vector<Type *> types;
  std::vector<std::string> names;
  std::string temp = "";
  while (std::regex_search(search_start, s.cend(), m, e)) {
    if (m[2] != "") {
      temp = m[2];
    } else {
      temp = m[7];
    }

    if (temp[0] == '|') {
      names.push_back(temp.substr(1, temp.length() - 2));
    } else {
      names.push_back(temp);
    }

    if (m[3] == "Bool" || m[8] == "Bool") // Boolean
    {
      types.push_back(Type::getInt1Ty(lcx));
    } else if (m[6] != "") // Bitvector
    {
      types.push_back(Type::getIntNTy(lcx, stoi(m[6].str())));
    } else if (m[11] != "") {
      types.push_back(Type::getIntNTy(lcx, stoi(m[11].str())));
    } else if (m[4] != "" && m[5] != "") // General floating point
    {
      types.push_back(FloatingNode::ToFloatingType(lcx, names.back(),
                                                   stoi(m[4]) + stoi(m[5])));
    } else if (m[9] != "" && m[10] != "") {
      types.push_back(FloatingNode::ToFloatingType(lcx, names.back(),
                                                   stoi(m[9]) + stoi(m[10])));
    } else if (m[3] == "Float16" || m[3] == "Float32" || m[3] == "Float64" ||
               m[3] == "Float128") // Named floating points
    {
      types.push_back(FloatingNode::ToFloatingType(lcx, names.back(),
                                                   stoi(m[3].str().substr(5))));
    } else if (m[8] == "Float16" || m[8] == "Float32" || m[8] == "Float64" ||
               m[8] == "Float128") // Named floating points
    {
      types.push_back(FloatingNode::ToFloatingType(lcx, names.back(),
                                                   stoi(m[8].str().substr(5))));
    } else if (m[3] == "FPN" ||
               m[8] == "FPN") // Special case for the convention (define-sort
                              // FPN () (_ FloatingPoint 11 53)) in QF_FP
    {
      types.push_back(FloatingNode::ToFloatingType(lcx, names.back(), 64));
    } else {
      throw UnsupportedTypeException("unsupported SMT variable type",
                                     names.back());
    }
    search_start = m.suffix().first;
  }

  // Create function
  FunctionType *fnty = FunctionType::get(Type::getInt1Ty(lcx), types, false);
  function =
      Function::Create(fnty, Function::ExternalLinkage, func_name, lmodule);
  // Assign variable types
  int i = 0;
  for (auto &arg : function->args()) {
    arg.setName(names[i]);
    variables[names[i]] = &arg;
    i++;
  }

  contents = c.parse_string(t_string.c_str());
  assertions.reserve(contents.size());

  for (expr e : contents) {
    assertions.emplace_back(lcx, lmodule, builder, variables, &value_cache, e);
  }
}

void SMTFormula::ToLLVM() {
  value_cache.clear();
  BasicBlock *bb = BasicBlock::Create(lcx, "b", function);
  builder.SetInsertPoint(bb);

  if (assertions.size() == 0) {
    // Empty constraint is sat
    builder.CreateRet(ConstantInt::getBool(lcx, true));
  } else {
    std::vector<Value *> pending;
    pending.reserve(assertions.size());
    for (auto &assertion : assertions) {
      pending.push_back(assertion.ToLLVM());
    }

    // Conjunction of all assertions with pairwise reduction to avoid
    // creating a long left-deep chain.
    while (pending.size() > 1) {
      std::vector<Value *> next;
      next.reserve((pending.size() + 1) / 2);
      for (size_t i = 0; i < pending.size(); i += 2) {
        if (i + 1 < pending.size()) {
          next.push_back(builder.CreateAnd(pending[i], pending[i + 1]));
        } else {
          next.push_back(pending[i]);
        }
      }
      pending.swap(next);
    }

    builder.CreateRet(pending[0]);
  }
}
} // namespace SLOT
