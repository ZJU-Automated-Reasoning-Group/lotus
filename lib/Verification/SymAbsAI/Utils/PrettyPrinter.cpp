#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Core/repr.h"

#include <set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <z3++.h>

namespace // unnamed
{
std::string getSourceName(const llvm::Value *value,
                          std::set<const llvm::Value *> *forbidden) {
  assert(value != nullptr);
  using namespace llvm;
  if (forbidden->find(value) != forbidden->end())
    return "";

  std::string var_name = "";
  forbidden->insert(value);

  // Try to get the LLVM IR name directly
  if (value->hasName()) {
    var_name = value->getName().str();
  }

  // if this is a phi instruction, try finding names of arguments
  if (var_name == "") {
    auto *as_phi = llvm::dyn_cast<llvm::PHINode>(value);

    if (as_phi != nullptr) {
      for (unsigned i = 0; i < as_phi->getNumOperands(); i++) {
        auto arg_name = getSourceName(as_phi->getOperand(i), forbidden);

        if (var_name == "" || arg_name == var_name) {
          var_name = arg_name;
        } else {
          // if we find two different names for the same value, it's
          // safer to just back off and don't report anything
          var_name = "";
          break;
        }
      }
    }
  }

  forbidden->erase(forbidden->find(value));
  return var_name;
}

std::string getSourceName(const llvm::Value *value) {
  std::set<const llvm::Value *> forbidden;
  return getSourceName(value, &forbidden);
}
} // namespace

namespace symabs_ai {
namespace pp {
tex top("top", "\\top");
tex bottom("bottom", "\\bot");
tex rightarrow(" -> ", "\\ \\rightarrow\\ ");
tex in(" in ", "\\in");
} // namespace pp

PrettyPrinter::Entry::Entry(PrettyPrinter *pp, const std::string &class_name)
    : PP_(pp) {
  if (pp->OutputHTML_) {
    if (class_name.size() > 0) {
      PP_->Result_ << "<div class=\"abstract_value\" title=\""
                   << escapeHTML(class_name) << "\">";
    } else {
      PP_->Result_ << "<div class=\"abstract_value\">";
    }
  }
}

PrettyPrinter::Entry::~Entry() {
  if (PP_->OutputHTML_) {
    PP_->Result_ << "</div>";
  } else {
    PP_->Result_ << '\n';
  }
}

void symabs_ai::PrettyPrinter::outputFormula(
    const z3::expr &expr, const std::map<std::string, llvm::Value *> &var_map) {
  std::ostringstream sout;
  sout << expr;
  std::string expr_str = sout.str();

  if (!OutputHTML_) {
    *this << expr_str;
    return;
  }

  for (size_t i = 0; i < expr_str.length();) {
    // Find the longest matching entry form var_map. This works because
    // var_map is ordered with operator< for which "pre" < "prefix".
    const std::pair<const std::string, llvm::Value *> *match = nullptr;
    for (auto &p : var_map) {
      if (expr_str.substr(i, p.first.length()) == p.first)
        match = &p;
    }

    if (match != nullptr) {
      *this << match->second;
      i += match->first.length();
    } else {
      *this << expr_str[i];
      i++;
    }
  }
}

PrettyPrinter &PrettyPrinter::operator<<(const pp::tex &tex) {
  if (OutputHTML_) {
    Result_ << "\\(";
    *this << tex.Tex_;
    Result_ << "\\)";
  } else {
    *this << tex.Plaintext_;
  }

  return *this;
}

PrettyPrinter &PrettyPrinter::operator<<(const llvm::Value *value) {
  using namespace llvm;

  if (!OutputHTML_)
    return *this << repr(value);

  Result_ << "<span class=\"source_name\">" << escapeHTML(getSourceName(value))
          << "</span>";

  Result_ << "<span class=\"llvmir_name\">"
          << escapeHTML(value->getName().str()) << "</span>";

  return *this;
}

PrettyPrinter &PrettyPrinter::operator<<(const std::string &x) {
  if (OutputHTML_) {
    Result_ << escapeHTML(x);
  } else {
    Result_ << x;
  }

  return *this;
}

JsonAnnotationOutput::JsonAnnotationOutput(std::ostream *out,
                                           const llvm::Function *func)
    : Out_(out) {
  *Out_ << "{" << '\n';
  std::string filename = getFunctionSourcePath(func);
  if (filename.size()) {
    *Out_ << "\"source\": \"" << escapeJSON(filename) << "\"," << '\n';
  }

  *Out_ << "\"annotations\": [" << '\n';
}

void JsonAnnotationOutput::emit(const std::string &annotation, int line,
                                int col) {
  if (NeedsComma_)
    *Out_ << "," << '\n';
  else
    NeedsComma_ = true;

  *Out_ << "{ \"annotation\": \"" << escapeJSON(annotation) << "\"";
  *Out_ << ", \"line\":" << line;

  if (col >= 0)
    *Out_ << ", \"column\":" << col;

  *Out_ << "}";
}

void JsonAnnotationOutput::emit(const AbstractValue *aval, int line, int col) {
  PrettyPrinter pp(true);
  aval->prettyPrint(pp);
  emit(pp.str(), line, col);
}

JsonAnnotationOutput::~JsonAnnotationOutput() { *Out_ << "]" << '\n' << "}"; }
} // namespace symabs_ai
