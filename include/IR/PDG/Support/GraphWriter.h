#pragma once

#include "llvm/ADT/GraphTraits.h"
#include "llvm/Analysis/DOTGraphTraitsPass.h"

#include "IR/PDG/Support/GraphTraits.h"

namespace llvm {
template <> struct DOTGraphTraits<pdg::Node *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}
};

template <>
struct DOTGraphTraits<pdg::ProgramDependencyGraph *>
    : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  // Return graph name;
  static std::string getGraphName(pdg::ProgramDependencyGraph *) {
    return "Program Dependency Graph";
  }

  static std::string renderInstructionLabel(Value *node_val) {
    if (Instruction *i = dyn_cast<Instruction>(node_val)) {
      std::string str;
      raw_string_ostream OS(str);
      OS << *i;
      return OS.str();
    }
    return "";
  }

  static std::string renderPrefixedValueLabel(StringRef prefix, Value *node_val) {
    if (!node_val)
      return prefix.str();
    std::string str;
    raw_string_ostream OS(str);
    OS << prefix << *node_val;
    return OS.str();
  }

  std::string getCommonNodeLabel(pdg::Node *node, bool include_param_nodes) {
    pdg::GraphNodeType node_type = node->getNodeType();
    Function *func = node->getFunc();
    Value *node_val = node->getValue();

    switch (node_type) {
    case pdg::GraphNodeType::FUNC_ENTRY:
      return func ? "<<ENTRY>> " + func->getName().str() : "<<ENTRY>>";
    case pdg::GraphNodeType::PARAM_FORMALIN:
      if (include_param_nodes) {
        std::string str;
        raw_string_ostream OS(str);
        pdg::pdgutils::printTreeNodesLabel(node, OS, "FORMAL_IN");
        return OS.str();
      }
      break;
    case pdg::GraphNodeType::PARAM_FORMALOUT:
      if (include_param_nodes) {
        std::string str;
        raw_string_ostream OS(str);
        pdg::pdgutils::printTreeNodesLabel(node, OS, "FORMAL_OUT");
        return OS.str();
      }
      break;
    case pdg::GraphNodeType::PARAM_ACTUALIN:
      if (include_param_nodes) {
        std::string str;
        raw_string_ostream OS(str);
        pdg::pdgutils::printTreeNodesLabel(node, OS, "ACTUAL_IN");
        return OS.str();
      }
      break;
    case pdg::GraphNodeType::PARAM_ACTUALOUT:
      if (include_param_nodes) {
        std::string str;
        raw_string_ostream OS(str);
        pdg::pdgutils::printTreeNodesLabel(node, OS, "ACTUAL_OUT");
        return OS.str();
      }
      break;
    case pdg::GraphNodeType::INST_OTHER:
    case pdg::GraphNodeType::INST_FUNCALL:
    case pdg::GraphNodeType::INST_RET:
    case pdg::GraphNodeType::INST_BR:
      return renderInstructionLabel(node_val);
    case pdg::GraphNodeType::ANNO_VAR:
      return renderPrefixedValueLabel("Local Anno: ", node_val);
    case pdg::GraphNodeType::ANNO_GLOBAL:
      return renderPrefixedValueLabel("Global Anno: ", node_val);
    case pdg::GraphNodeType::VAR_STATICALLOCGLOBALSCOPE:
      return renderPrefixedValueLabel("global var: ", node_val);
    case pdg::GraphNodeType::VAR_STATICALLOCMODULESCOPE:
      return renderPrefixedValueLabel("static global var: ", node_val);
    case pdg::GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE:
      return renderPrefixedValueLabel("static func var: ", node_val);
    case pdg::GraphNodeType::CLASS: {
      auto *node_di_type = node->getDIType();
      std::string class_type_name = "unknown";
      if (node_di_type != nullptr)
        class_type_name = pdg::dbgutils::getSourceLevelTypeName(*node_di_type);
      return class_type_name;
    }
    default:
      break;
    }

    return "";
  }

  std::string getCDGNodeLabel(pdg::Node *node) {
    return getCommonNodeLabel(node, false);
  }

  std::string getDDGNodeLabel(pdg::Node *node) {
    return getCommonNodeLabel(node, false);
  }

  std::string getNodeLabel(pdg::Node *node, pdg::ProgramDependencyGraph *G) {
    if (pdg::DOTONLYDDG)
      return getDDGNodeLabel(node);
    if (pdg::DOTONLYCDG)
      return getCDGNodeLabel(node);
    return getCommonNodeLabel(node, true);
  }

  std::string getDDGEdgeAttributes(pdg::Node::iterator edge_iter) {
    pdg::EdgeType edge_type = edge_iter.getEdgeType();
    switch (edge_type) {
    case pdg::EdgeType::DATA_DEF_USE:
      return "style=dotted,label = \"{D_DEF_USE}\" ";
    case pdg::EdgeType::DATA_ALIAS:
      return "style=dotted,label = \"{D_ALIAS}\" ";
    case pdg::EdgeType::DATA_RAW:
      return "style=dotted,label = \"{D_RAW}\" ";
    case pdg::EdgeType::DATA_RET:
      return "style=dashed, color=\"red\", label =\"{D_RET}\"";
    case pdg::EdgeType::ANNO_GLOBAL:
      return "style=dashed, color=\"green\", label =\"{ANNO_GLOB}\"";
    case pdg::EdgeType::ANNO_VAR:
      return "style=dashed, color=\"green\", label =\"{ANNO_VAR}\"";
    default:
      break;
    }
    return "style=invis";
  }

  std::string getEdgeAttributes(pdg::Node *Node, pdg::Node::iterator edge_iter,
                                pdg::ProgramDependencyGraph *PDG) {
    if (pdg::DOTONLYDDG)
      return getDDGEdgeAttributes(edge_iter);
    pdg::EdgeType edge_type = edge_iter.getEdgeType();
    switch (edge_type) {
    case pdg::EdgeType::CONTROLDEP_ENTRY:
      return "label = \"{CONTROLDEP_ENTRY}\"";
    case pdg::EdgeType::CONTROLDEP_BR:
      return "label = \"{CONTROLDEP_BR}\"";
    case pdg::EdgeType::CONTROLDEP_CALLINV:
      return "label = \"{CONTROLDEP_CALLINV}\"";
    case pdg::EdgeType::CONTROLDEP_CALLRET:
      return "label = \"{CONTROLDEP_CALLRET}\"";
    case pdg::EdgeType::CONTROLDEP_IND_BR:
      return "label = \"{CONTROLDEP_IND_BR}\"";
    case pdg::EdgeType::DATA_DEF_USE:
      return "style=dotted,label = \"{D_DEF_USE}\" ";
    case pdg::EdgeType::DATA_ALIAS:
      return "style=dotted,label = \"{D_ALIAS}\" ";
    case pdg::EdgeType::PARAMETER_IN:
      return "style=dashed, color=\"blue\", label=\"{P_IN}\"";
    case pdg::EdgeType::PARAMETER_OUT:
      return "style=dashed, color=\"blue\", label=\"{P_OUT}\"";
    case pdg::EdgeType::PARAMETER_FIELD:
      return "style=dashed, color=\"blue\", label=\"{P_F}\"";
    case pdg::EdgeType::DATA_RAW:
      return "style=dotted,label = \"{D_RAW}\" ";
    case pdg::EdgeType::DATA_RET:
      return "style=dashed, color=\"red\", label =\"{D_RET}\"";
    case pdg::EdgeType::ANNO_GLOBAL:
      return "style=dashed, color=\"green\", label =\"{ANNO_GLOB}\"";
    case pdg::EdgeType::ANNO_VAR:
      return "style=dashed, color=\"green\", label =\"{ANNO_VAR}\"";
    case pdg::EdgeType::CLS_MTH:
      return "style=dashed, color=\"red\", label =\"{CLS_MTH}\"";
    default:
      break;
    }
    return "";
  }
};
} // namespace llvm

namespace pdg {
struct ProgramDependencyPrinter
    : public llvm::DOTGraphTraitsPrinter<ProgramDependencyGraph, false> {
  static char ID;
  ProgramDependencyPrinter()
      : llvm::DOTGraphTraitsPrinter<ProgramDependencyGraph, false>("pdggraph",
                                                                   ID) {}
};

} // namespace pdg
