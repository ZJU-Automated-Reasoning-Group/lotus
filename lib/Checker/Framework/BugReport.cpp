// Author: rainoftime
#include "Checker/Framework/BugReport.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include <algorithm>
#include <cctype>
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

static bool hasNodeTag(const BugDiagStep &step, NodeTag tag) {
  return std::find(step.node_tags.begin(), step.node_tags.end(), tag) !=
         step.node_tags.end();
}

static bool startsWithTagNarrative(StringRef text) {
  return text.startswith("Enter function ") ||
         text.startswith("Return from function ") ||
         text.startswith("Select the true branch") ||
         text.startswith("Select the false branch") ||
         text.startswith("Begin procedure ") ||
         text.startswith("Exit procedure ") ||
         text.startswith("Exceptional control flow") ||
         text.startswith("Access ");
}

static bool isSuggestionText(StringRef text) {
  return text.ltrim().startswith("Suggestion:");
}

static std::string normalizeSnippet(StringRef text, size_t maxLen = 140) {
  std::string result;
  result.reserve(std::min(text.size(), maxLen));

  bool previousWasSpace = false;
  for (char c : text.trim()) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!result.empty() && !previousWasSpace) {
        result.push_back(' ');
      }
      previousWasSpace = true;
      continue;
    }

    previousWasSpace = false;
    result.push_back(c);
    if (result.size() >= maxLen) {
      result.append("...");
      break;
    }
  }

  return result;
}

static std::string renderSourceLead(const BugDiagStep &step) {
  std::string source = normalizeSnippet(step.source_code);
  if (!source.empty()) {
    return "Source: " + source + ". ";
  }

  if (!step.var_name.empty()) {
    return "Variable: " + step.var_name + ". ";
  }

  return "";
}

static std::string joinNarrativeParts(const std::vector<std::string> &parts) {
  std::string out;
  for (const auto &part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += ". ";
    }
    out += part;
  }
  return out;
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

std::string BugReport::render_step_message(const BugDiagStep &step) const {
  std::vector<std::string> narrativeParts;

  if (hasNodeTag(step, NodeTag::CALL_SITE)) {
    if (!step.func_name.empty()) {
      narrativeParts.push_back("Enter function " + step.func_name);
    } else {
      narrativeParts.push_back("Enter function call");
    }
  }

  if (hasNodeTag(step, NodeTag::RETURN_SITE)) {
    if (!step.func_name.empty()) {
      narrativeParts.push_back("Return from function " + step.func_name);
    } else {
      narrativeParts.push_back("Return from function call");
    }
  }

  if (hasNodeTag(step, NodeTag::CONDITION_TRUE)) {
    narrativeParts.push_back("Select the true branch at this point");
  }

  if (hasNodeTag(step, NodeTag::CONDITION_FALSE)) {
    narrativeParts.push_back("Select the false branch at this point");
  }

  if (hasNodeTag(step, NodeTag::PROCEDURE_START) && !step.func_name.empty()) {
    narrativeParts.push_back("Begin procedure " + step.func_name);
  }

  if (hasNodeTag(step, NodeTag::PROCEDURE_END) && !step.func_name.empty()) {
    narrativeParts.push_back("Exit procedure " + step.func_name);
  }

  if (hasNodeTag(step, NodeTag::EXCEPTION)) {
    narrativeParts.push_back("Exceptional control flow reaches this point");
  }

  if (!step.access.empty()) {
    if (!step.var_name.empty()) {
      narrativeParts.push_back("Access " + step.access + " through " +
                               step.var_name);
    } else {
      narrativeParts.push_back("Access " + step.access);
    }
  }

  std::string narrative = joinNarrativeParts(narrativeParts);
  StringRef tip(step.tip);
  std::string sourceLead = renderSourceLead(step);

  if (tip.empty()) {
    if (!sourceLead.empty() && !narrative.empty()) {
      return sourceLead + narrative;
    }
    return narrative.empty() ? sourceLead : narrative;
  }

  std::string body;
  if (narrative.empty() || tip == narrative || startsWithTagNarrative(tip)) {
    body = step.tip;
  } else {
    body = narrative + ". " + step.tip;
  }

  if (sourceLead.empty() || isSuggestionText(tip)) {
    return body;
  }

  return sourceLead + body;
}

std::string BugReport::render_primary_message() const {
  const BugDiagStep *fallback = nullptr;

  for (auto it = trigger_steps.rbegin(); it != trigger_steps.rend(); ++it) {
    if (*it == nullptr) {
      continue;
    }
    if (fallback == nullptr) {
      fallback = *it;
    }
    if (isSuggestionText((*it)->tip)) {
      continue;
    }
    std::string rendered = render_step_message(**it);
    if (!rendered.empty()) {
      return rendered;
    }
  }

  if (fallback != nullptr) {
    return render_step_message(*fallback);
  }
  return "";
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

cJSON *BugReport::toJson() const {
  cJSON *report = cJSON_CreateObject();
  if (!report) {
    return nullptr;
  }

  cJSON_AddBoolToObject(report, "Dominated", dominated);
  cJSON_AddBoolToObject(report, "Valid", valid);
  cJSON_AddNumberToObject(report, "Score", conf_score);
  cJSON_AddNumberToObject(report, "Session", session);

  if (extras) {
    if (!extras->suggestion.empty()) {
      cJSON_AddStringToObject(report, "Suggestion", extras->suggestion.c_str());
    }
    if (!extras->metadata.empty()) {
      cJSON *metadata = cJSON_AddObjectToObject(report, "Metadata");
      for (const auto &pair : extras->metadata) {
        cJSON_AddStringToObject(metadata, pair.first.c_str(),
                                pair.second.c_str());
      }
    }
  }

  cJSON *diagSteps = cJSON_AddArrayToObject(report, "DiagSteps");
  for (const BugDiagStep *step : trigger_steps) {
    if (!step) {
      continue;
    }
    cJSON *diagStep = cJSON_CreateObject();
    cJSON_AddItemToArray(diagSteps, diagStep);

    if (!step->src_file.empty()) {
      cJSON_AddStringToObject(diagStep, "File", step->src_file.c_str());
      cJSON_AddNumberToObject(diagStep, "Line", step->src_line);
      if (step->src_column > 0) {
        cJSON_AddNumberToObject(diagStep, "Column", step->src_column);
      }
    }

    if (!step->func_name.empty()) {
      cJSON_AddStringToObject(diagStep, "Function", step->func_name.c_str());
    }

    if (!step->var_name.empty()) {
      cJSON_AddStringToObject(diagStep, "Variable", step->var_name.c_str());
    }

    if (!step->type_name.empty()) {
      cJSON_AddStringToObject(diagStep, "Type", step->type_name.c_str());
    }

    if (!step->source_code.empty()) {
      cJSON_AddStringToObject(diagStep, "SourceCode",
                              step->source_code.c_str());
    }

    if (!step->llvm_ir.empty()) {
      cJSON_AddStringToObject(diagStep, "LLVM_IR", step->llvm_ir.c_str());
    }

    if (step->trace_level > 0) {
      cJSON_AddNumberToObject(diagStep, "TraceLevel", step->trace_level);
    }

    if (!step->node_tags.empty()) {
      cJSON *nodeTags = cJSON_AddArrayToObject(diagStep, "NodeTags");
      for (NodeTag tag : step->node_tags) {
        std::string tagName = nodeTagToString(tag);
        cJSON_AddItemToArray(nodeTags, cJSON_CreateString(tagName.c_str()));
      }
    }

    if (!step->access.empty()) {
      cJSON_AddStringToObject(diagStep, "Access", step->access.c_str());
    }

    if (step->node_id >= 0) {
      cJSON_AddNumberToObject(diagStep, "NodeID", step->node_id);
    }

    std::string renderedMessage = render_step_message(*step);
    if (!renderedMessage.empty()) {
      cJSON_AddStringToObject(diagStep, "Narrative", renderedMessage.c_str());
    }

    cJSON_AddStringToObject(diagStep, "Tip", step->tip.c_str());
  }

  return report;
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
