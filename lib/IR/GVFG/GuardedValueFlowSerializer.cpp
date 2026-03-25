#include "IR/GVFG/GuardedValueFlowSerializer.h"

#include "IR/GVFG/GuardedValueFlowGraph.h"

#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::gvfg;

namespace {

static std::string escapeDotLabel(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (char ch : input) {
    switch (ch) {
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      out.push_back(ch);
      break;
    }
  }
  return out;
}

static std::string renderValue(const Value *value) {
  if (!value)
    return "<none>";

  std::string buffer;
  raw_string_ostream os(buffer);
  value->printAsOperand(os, false);
  return os.str();
}

static std::string renderType(const Type *type) {
  if (!type)
    return "<none>";

  std::string buffer;
  raw_string_ostream os(buffer);
  type->print(os);
  return os.str();
}

static std::string renderInstruction(const Instruction *inst) {
  if (!inst)
    return "<none>";

  std::string buffer;
  raw_string_ostream os(buffer);
  inst->print(os);
  return os.str();
}

static std::string renderBlockName(const BasicBlock *block) {
  if (!block)
    return "<none>";
  if (block->hasName())
    return block->getName().str();

  std::string buffer;
  raw_string_ostream os(buffer);
  block->printAsOperand(os, false);
  return os.str();
}

static const char *nodeKindName(GuardedValueFlowNode::Kind kind) {
  switch (kind) {
  case GuardedValueFlowNode::Kind::CommonArgument:
    return "CommonArgument";
  case GuardedValueFlowNode::Kind::PseudoArgument:
    return "PseudoArgument";
  case GuardedValueFlowNode::Kind::VariableArgument:
    return "VariableArgument";
  case GuardedValueFlowNode::Kind::CommonReturn:
    return "CommonReturn";
  case GuardedValueFlowNode::Kind::PseudoReturn:
    return "PseudoReturn";
  case GuardedValueFlowNode::Kind::SimpleOperand:
    return "SimpleOperand";
  case GuardedValueFlowNode::Kind::UndefValue:
    return "UndefValue";
  case GuardedValueFlowNode::Kind::LoadMemory:
    return "LoadMemory";
  case GuardedValueFlowNode::Kind::StoreMemory:
    return "StoreMemory";
  case GuardedValueFlowNode::Kind::Phi:
    return "Phi";
  case GuardedValueFlowNode::Kind::Region:
    return "Region";
  case GuardedValueFlowNode::Kind::CallSiteCommonOutput:
    return "CallSiteCommonOutput";
  case GuardedValueFlowNode::Kind::CallSitePseudoOutput:
    return "CallSitePseudoOutput";
  case GuardedValueFlowNode::Kind::CallSitePseudoInput:
    return "CallSitePseudoInput";
  case GuardedValueFlowNode::Kind::CallSiteArgumentSummary:
    return "CallSiteArgumentSummary";
  case GuardedValueFlowNode::Kind::CallSiteReturnSummary:
    return "CallSiteReturnSummary";
  case GuardedValueFlowNode::Kind::InterfaceCondition:
    return "InterfaceCondition";
  case GuardedValueFlowNode::Kind::SimpleOpcode:
    return "SimpleOpcode";
  case GuardedValueFlowNode::Kind::CastOpcode:
    return "CastOpcode";
  case GuardedValueFlowNode::Kind::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

static const char *siteKindName(GuardedValueFlowSite::Kind kind) {
  switch (kind) {
  case GuardedValueFlowSite::Kind::CallSite:
    return "CallSite";
  case GuardedValueFlowSite::Kind::ReturnSite:
    return "ReturnSite";
  case GuardedValueFlowSite::Kind::DereferenceSite:
    return "DereferenceSite";
  case GuardedValueFlowSite::Kind::GEP:
    return "GEP";
  case GuardedValueFlowSite::Kind::Compare:
    return "Compare";
  case GuardedValueFlowSite::Kind::Div:
    return "Div";
  case GuardedValueFlowSite::Kind::Alloc:
    return "Alloc";
  case GuardedValueFlowSite::Kind::Unknown:
    return "Unknown";
  }
  return "Unknown";
}

static const char *
diagnosticOriginName(GuardedValueFlowGraph::Diagnostic::Origin origin) {
  switch (origin) {
  case GuardedValueFlowGraph::Diagnostic::Origin::Builder:
    return "builder";
  case GuardedValueFlowGraph::Diagnostic::Origin::Adapter:
    return "adapter";
  }
  return "builder";
}

static const char *
diagnosticSeverityName(GuardedValueFlowGraph::Diagnostic::Severity severity) {
  switch (severity) {
  case GuardedValueFlowGraph::Diagnostic::Severity::Note:
    return "note";
  case GuardedValueFlowGraph::Diagnostic::Severity::Warning:
    return "warning";
  case GuardedValueFlowGraph::Diagnostic::Severity::Error:
    return "error";
  }
  return "warning";
}

static std::string renderAccessPath(const AccessPath &path) {
  if (path.empty())
    return "<none>";

  std::ostringstream os;
  os << renderValue(path.getBase());
  for (int idx = 0; idx < path.getDepth(); ++idx)
    os << "." << path.getOffset(idx);
  if (path.isFromReturn())
    os << " [ret]";
  return os.str();
}

static const char *dotShapeForNode(const GuardedValueFlowNode *node) {
  if (!node)
    return "box";

  switch (node->getKind()) {
  case GuardedValueFlowNode::Kind::Region:
    return "diamond";
  case GuardedValueFlowNode::Kind::LoadMemory:
  case GuardedValueFlowNode::Kind::StoreMemory:
    return "box3d";
  case GuardedValueFlowNode::Kind::Phi:
    return "hexagon";
  case GuardedValueFlowNode::Kind::Unknown:
    return "octagon";
  case GuardedValueFlowNode::Kind::SimpleOpcode:
  case GuardedValueFlowNode::Kind::CastOpcode:
    return "ellipse";
  default:
    return "box";
  }
}

} // namespace

std::string
GuardedValueFlowSerializer::toText(const GuardedValueFlowGraph &graph) {
  std::ostringstream out;
  out << "GVFG-TEXT-V1\n";
  out << "function "
      << std::quoted(graph.getBaseFunction()
                         ? graph.getBaseFunction()->getName().str()
                         : std::string("<none>"))
      << "\n";
  out << "diagnostics " << graph.diagnostics().size() << "\n";
  for (const auto &diagnostic : graph.diagnostics()) {
    out << "diagnostic " << diagnosticOriginName(diagnostic.origin) << " "
        << diagnosticSeverityName(diagnostic.severity) << " "
        << std::quoted(diagnostic.message) << " "
        << std::quoted(renderBlockName(diagnostic.block)) << " "
        << std::quoted(renderInstruction(diagnostic.instruction)) << "\n";
  }

  out << "nodes " << graph.nodes().size() << "\n";
  for (const auto &node_ptr : graph.nodes()) {
    const auto *node = node_ptr.get();
    out << "node " << node->getNodeId() << " " << nodeKindName(node->getKind())
        << " " << std::quoted(node->getDescription()) << " "
        << std::quoted(renderType(node->getType())) << " "
        << std::quoted(renderBlockName(node->getParentBasicBlock())) << " "
        << std::quoted(renderValue(node->getLLVMValue())) << " "
        << std::quoted(renderAccessPath(node->getAccessPath())) << " "
        << node->getIndex() << "\n";
  }

  size_t edge_count = 0;
  for (const auto &node_ptr : graph.nodes())
    edge_count += node_ptr->children().size();
  out << "edges " << edge_count << "\n";
  for (const auto &node_ptr : graph.nodes()) {
    const auto *node = node_ptr.get();
    for (const auto &edge : node->children()) {
      out << "edge " << node->getNodeId() << " "
          << (edge.target ? edge.target->getNodeId() : 0) << " " << std::fixed
          << std::setprecision(3) << edge.confidence << " "
          << std::quoted(edge.condition.render()) << "\n";
    }
  }

  out << "sites " << graph.sites().size() << "\n";
  for (size_t idx = 0; idx < graph.sites().size(); ++idx) {
    const auto *site = graph.sites()[idx].get();
    out << "site " << idx << " " << siteKindName(site->getKind()) << " "
        << std::quoted(renderInstruction(site->getInstruction())) << "\n";
  }

  return out.str();
}

std::string
GuardedValueFlowSerializer::toDot(const GuardedValueFlowGraph &graph) {
  std::ostringstream out;
  const std::string function_name =
      graph.getBaseFunction() ? graph.getBaseFunction()->getName().str()
                              : "<none>";

  out << "digraph \"gvfg." << escapeDotLabel(function_name) << "\" {\n";
  out << "  rankdir=LR;\n";
  out << "  labelloc=t;\n";
  out << "  label=\"GVFG: " << escapeDotLabel(function_name)
      << "\\nDiagnostics: " << graph.diagnostics().size() << "\";\n";

  for (const auto &node_ptr : graph.nodes()) {
    const auto *node = node_ptr.get();
    std::ostringstream label;
    label << "#" << node->getNodeId() << "\\n" << nodeKindName(node->getKind());
    if (!node->getDescription().empty())
      label << "\\n" << escapeDotLabel(node->getDescription());
    if (node->getParentBasicBlock())
      label << "\\nBB="
            << escapeDotLabel(renderBlockName(node->getParentBasicBlock()));
    out << "  n" << node->getNodeId() << " [shape=" << dotShapeForNode(node)
        << ", label=\"" << label.str() << "\"];\n";
  }

  for (const auto &node_ptr : graph.nodes()) {
    const auto *node = node_ptr.get();
    for (const auto &edge : node->children()) {
      if (!edge.target)
        continue;
      std::ostringstream label;
      if (edge.confidence != 1.0f)
        label << std::fixed << std::setprecision(2) << edge.confidence;
      if (edge.condition.isValid()) {
        if (!label.str().empty())
          label << " ";
        label << edge.condition.render();
      }
      out << "  n" << node->getNodeId() << " -> n" << edge.target->getNodeId();
      if (!label.str().empty())
        out << " [label=\"" << escapeDotLabel(label.str()) << "\"]";
      out << ";\n";
    }
  }

  std::map<const GuardedValueFlowSite *, size_t> site_ids;
  for (size_t idx = 0; idx < graph.sites().size(); ++idx)
    site_ids[graph.sites()[idx].get()] = idx;

  for (const auto &site_entry : site_ids) {
    const auto *site = site_entry.first;
    const size_t site_id = site_entry.second;
    std::ostringstream label;
    label << siteKindName(site->getKind());
    if (site->getInstruction())
      label << "\\n"
            << escapeDotLabel(renderInstruction(site->getInstruction()));
    out << "  s" << site_id
        << " [shape=note, style=dashed, color=gray40, label=\"" << label.str()
        << "\"];\n";
  }

  for (const auto &node_ptr : graph.nodes()) {
    const auto *node = node_ptr.get();
    for (GuardedValueFlowSite *site : node->useSites()) {
      auto site_it = site_ids.find(site);
      if (site_it == site_ids.end())
        continue;
      out << "  n" << node->getNodeId() << " -> s" << site_it->second
          << " [style=dotted, color=gray50, label=\"uses\"];\n";
    }
  }

  for (size_t idx = 0; idx < graph.diagnostics().size(); ++idx) {
    const auto &diagnostic = graph.diagnostics()[idx];
    std::ostringstream label;
    label << diagnosticOriginName(diagnostic.origin) << " "
          << diagnosticSeverityName(diagnostic.severity) << "\\n"
          << escapeDotLabel(diagnostic.message);
    out << "  d" << idx << " [shape=note, color=red, fontcolor=red, label=\""
        << label.str() << "\"];\n";
  }

  out << "}\n";
  return out.str();
}

bool GuardedValueFlowSerializer::writeText(const GuardedValueFlowGraph &graph,
                                           const std::string &filename) {
  std::ofstream out(filename);
  if (!out.is_open())
    return false;
  out << toText(graph);
  return out.good();
}

bool GuardedValueFlowSerializer::writeDot(const GuardedValueFlowGraph &graph,
                                          const std::string &filename) {
  std::ofstream out(filename);
  if (!out.is_open())
    return false;
  out << toDot(graph);
  return out.good();
}
