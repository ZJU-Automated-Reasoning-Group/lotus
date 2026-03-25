// Author: rainoftime
#include "Checker/Report/BugReport.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include <functional>
#include <unordered_set>

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugLoc.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// Shared DebugInfoAnalysis instance (one for all bug reports)
static DebugInfoAnalysis debugInfo;

// Helper to convert NodeTag to string
static std::string nodeTagToString(NodeTag tag) {
  switch (tag) {
  case NodeTag::CONDITION_TRUE:
    return "CONDITION_TRUE";
  case NodeTag::CONDITION_FALSE:
    return "CONDITION_FALSE";
  case NodeTag::EXCEPTION:
    return "EXCEPTION";
  case NodeTag::PROCEDURE_START:
    return "PROCEDURE_START";
  case NodeTag::PROCEDURE_END:
    return "PROCEDURE_END";
  case NodeTag::CALL_SITE:
    return "CALL_SITE";
  case NodeTag::RETURN_SITE:
    return "RETURN_SITE";
  default:
    return "NONE";
  }
}

// Helper to infer node tags from instruction type
static std::vector<NodeTag> inferNodeTags(Instruction *I) {
  std::vector<NodeTag> tags;
  if (isa<CallInst>(I)) {
    tags.push_back(NodeTag::CALL_SITE);
  } else if (isa<ReturnInst>(I)) {
    tags.push_back(NodeTag::RETURN_SITE);
  } else if (isa<BranchInst>(I)) {
    // Could be condition, but we don't know true/false without analysis
    // Leave empty for now, checkers can add explicitly
  }
  return tags;
}

void BugReport::append_step(Value *inst, const std::string &tip,
                            int trace_level, const std::vector<NodeTag> &tags,
                            const std::string &access) {
  BugDiagStep *step = new BugDiagStep();
  step->inst = inst;
  step->tip = tip;
  step->trace_level = trace_level;
  step->node_tags = tags;
  step->access = access;

  // Extract LLVM IR representation
  if (inst) {
    std::string ir_str;
    raw_string_ostream ir_os(ir_str);
    inst->print(ir_os);
    step->llvm_ir = ir_os.str();

    // Extract variable name using DebugInfoAnalysis
    step->var_name = debugInfo.getVariableName(inst);

    // Extract type information
    step->type_name = debugInfo.getTypeName(inst);
  }

  // Extract debug information if available
  if (auto *I = dyn_cast_or_null<Instruction>(inst)) {
    // Get source location components using DebugInfoAnalysis
    step->src_file = debugInfo.getSourceFile(I);
    step->src_line = debugInfo.getSourceLine(I);
    step->src_column = debugInfo.getSourceColumn(I);

    // Get function name using DebugInfoAnalysis (includes demangling)
    step->func_name = debugInfo.getFunctionName(I);

    // Extract the actual source code statement using DebugInfoAnalysis
    step->source_code = debugInfo.getSourceCodeStatement(I);

    // Infer node tags from instruction type if not provided
    if (tags.empty()) {
      step->node_tags = inferNodeTags(I);
    }
  }

  trigger_steps.push_back(step);
}

// Helper to escape JSON strings
static std::string escapeJSON(const std::string &str) {
  std::string escaped;
  for (char c : str) {
    switch (c) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (c < 32) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
        escaped += buf;
      } else {
        escaped += c;
      }
    }
  }
  return escaped;
}

void BugReport::set_suggestion(const std::string &suggestion) {
  if (!extras) {
    extras = new BugReportExtras();
  }
  extras->suggestion = suggestion;
}

void BugReport::add_metadata(const std::string &key, const std::string &value) {
  if (!extras) {
    extras = new BugReportExtras();
  }
  extras->metadata[key] = value;
}

size_t BugReport::compute_hash(bool use_trace) const {
  std::hash<std::string> hasher;
  size_t hash = 0;

  if (use_trace) {
    // Hash based on trace (location sequence)
    for (const BugDiagStep *step : trigger_steps) {
      hash ^= hasher(step->src_file) << 1;
      hash ^= std::hash<int>{}(step->src_line) << 2;
      hash ^= hasher(step->tip) << 3;
    }
  } else {
    // Hash based on primary location only
    if (!trigger_steps.empty()) {
      const BugDiagStep *primary = trigger_steps[0];
      hash ^= hasher(primary->src_file) << 1;
      hash ^= std::hash<int>{}(primary->src_line) << 2;
    }
  }

  hash ^= std::hash<int>{}(bug_type_id) << 4;
  return hash;
}

void BugReport::export_json(raw_ostream &OS) const {
  OS << "    {\n";
  OS << "      \"Dominated\": " << (dominated ? "true" : "false") << ",\n";
  OS << "      \"Valid\": " << (valid ? "true" : "false") << ",\n";
  OS << "      \"Score\": " << conf_score << ",\n";
  OS << "      \"Session\": " << session << ",\n";

  // Export extras if available
  if (extras) {
    if (!extras->suggestion.empty()) {
      OS << "      \"Suggestion\": \"" << escapeJSON(extras->suggestion)
         << "\",\n";
    }
    if (!extras->metadata.empty()) {
      OS << "      \"Metadata\": {\n";
      bool first = true;
      for (const auto &pair : extras->metadata) {
        if (!first)
          OS << ",\n";
        first = false;
        OS << "        \"" << escapeJSON(pair.first) << "\": \""
           << escapeJSON(pair.second) << "\"";
      }
      OS << "\n      },\n";
    }
  }

  OS << "      \"DiagSteps\": [\n";

  for (size_t i = 0; i < trigger_steps.size(); ++i) {
    const BugDiagStep *step = trigger_steps[i];
    OS << "        {\n";

    if (!step->src_file.empty()) {
      OS << "          \"File\": \"" << step->src_file << "\",\n";
      OS << "          \"Line\": " << step->src_line << ",\n";
      if (step->src_column > 0) {
        OS << "          \"Column\": " << step->src_column << ",\n";
      }
    }

    if (!step->func_name.empty()) {
      OS << "          \"Function\": \"" << escapeJSON(step->func_name)
         << "\",\n";
    }

    if (!step->var_name.empty()) {
      OS << "          \"Variable\": \"" << escapeJSON(step->var_name)
         << "\",\n";
    }

    if (!step->type_name.empty()) {
      OS << "          \"Type\": \"" << escapeJSON(step->type_name) << "\",\n";
    }

    if (!step->source_code.empty()) {
      OS << "          \"SourceCode\": \"" << escapeJSON(step->source_code)
         << "\",\n";
    }

    if (!step->llvm_ir.empty()) {
      OS << "          \"LLVM_IR\": \"" << escapeJSON(step->llvm_ir) << "\",\n";
    }

    // Export new fields
    if (step->trace_level > 0) {
      OS << "          \"TraceLevel\": " << step->trace_level << ",\n";
    }

    if (!step->node_tags.empty()) {
      OS << "          \"NodeTags\": [";
      for (size_t j = 0; j < step->node_tags.size(); ++j) {
        if (j > 0)
          OS << ", ";
        OS << "\"" << nodeTagToString(step->node_tags[j]) << "\"";
      }
      OS << "],\n";
    }

    if (!step->access.empty()) {
      OS << "          \"Access\": \"" << escapeJSON(step->access) << "\",\n";
    }

    if (step->node_id >= 0) {
      OS << "          \"NodeID\": " << step->node_id << ",\n";
    }

    OS << "          \"Tip\": \"" << escapeJSON(step->tip) << "\"\n";
    OS << "        }";

    if (i < trigger_steps.size() - 1) {
      OS << ",";
    }
    OS << "\n";
  }

  OS << "      ]\n";
  OS << "    }";
}

// Print a formatted bug report with debug information
void printBugReport(const llvm::Instruction *BugInst,
                    const std::string &BugType,
                    const llvm::Value *RelatedValue) {
  printf("[BUG REPORT] %s\n", BugType.c_str());
  printf("  Location: %s\n", debugInfo.getSourceLocation(BugInst).c_str());
  printf("  Function: %s\n", debugInfo.getFunctionName(BugInst).c_str());
  if (RelatedValue) {
    printf("  Variable: %s\n", debugInfo.getVariableName(RelatedValue).c_str());
    printf("  Type: %s\n", debugInfo.getTypeName(RelatedValue).c_str());
  }

  // Try to show source code
  std::string srcCode = debugInfo.getSourceCodeStatement(BugInst);
  if (!srcCode.empty()) {
    printf("  Source Code: %s\n", srcCode.c_str());
  }

  printf("\n");
}
