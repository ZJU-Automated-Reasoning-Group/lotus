#pragma once

#include "IR/GVFG/ConditionRef.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace lotus {
namespace gvfg {

using llvm::ArrayRef;
using llvm::Function;
using llvm::Instruction;

class GuardedValueFlowGraph;
class GuardedValueFlowNode;
class GuardedValueFlowRegionNode;

// Sites annotate instructions that are semantically important to later
// analyses. Nodes carry the value-flow graph; sites carry the original program
// operation and the operands that made that operation interesting.
class GuardedValueFlowSite {
public:
  enum class Kind {
    CallSite,
    ReturnSite,
    DereferenceSite,
    GEP,
    Compare,
    Div,
    Alloc,
    Unknown,
  };

  GuardedValueFlowSite(Kind kind, GuardedValueFlowGraph *graph,
                       Instruction *inst)
      : kind_(kind), graph_(graph), inst_(inst) {}
  virtual ~GuardedValueFlowSite() = default;

  Kind getKind() const { return kind_; }
  GuardedValueFlowGraph *getGraph() const { return graph_; }
  Instruction *getInstruction() const { return inst_; }

private:
  Kind kind_;
  GuardedValueFlowGraph *graph_;
  Instruction *inst_;
};

class GuardedValueFlowAllocSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowAllocSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Alloc, graph, inst) {}
};

class GuardedValueFlowCallSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowCallSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::CallSite, graph, inst) {}

  void addCallee(Function *callee) {
    if (!callee)
      return;
    if (std::find(callees_.begin(), callees_.end(), callee) == callees_.end())
      callees_.push_back(callee);
  }
  ArrayRef<Function *> getCallees() const { return callees_; }

  void addCommonInput(GuardedValueFlowNode *node);
  ArrayRef<GuardedValueFlowNode *> getCommonInputs() const {
    return common_inputs_;
  }

  // CommonOutput is the direct non-void call result. Pseudo inputs/outputs are
  // keyed by callee because different resolved callees may expose different
  // side-effect channels.
  void setCommonOutput(GuardedValueFlowNode *node) { common_output_ = node; }
  GuardedValueFlowNode *getCommonOutput() const { return common_output_; }

  void addPseudoInput(Function *callee, GuardedValueFlowNode *node);
  void addPseudoOutput(Function *callee, GuardedValueFlowNode *node);
  void setInputSummaryNode(Function *callee, unsigned summary_index,
                           GuardedValueFlowNode *node) {
    input_summary_nodes_[std::make_pair(callee, summary_index)] = node;
  }
  GuardedValueFlowNode *getInputSummaryNode(Function *callee,
                                            unsigned summary_index) const {
    auto it = input_summary_nodes_.find(std::make_pair(callee, summary_index));
    return it == input_summary_nodes_.end() ? nullptr : it->second;
  }
  void setOutputSummaryNode(Function *callee, unsigned summary_index,
                            GuardedValueFlowNode *node) {
    output_summary_nodes_[std::make_pair(callee, summary_index)] = node;
  }
  GuardedValueFlowNode *getOutputSummaryNode(Function *callee,
                                             unsigned summary_index) const {
    auto it = output_summary_nodes_.find(std::make_pair(callee, summary_index));
    return it == output_summary_nodes_.end() ? nullptr : it->second;
  }
  void setBackEdge(Function *callee) {
    if (callee)
      back_edge_callees_.insert(callee);
  }
  bool isBackEdge(Function *callee) const {
    return callee && back_edge_callees_.count(callee) != 0;
  }
  void setCalleeCondition(Function *callee, ConditionRef condition,
                          GuardedValueFlowRegionNode *region = nullptr);
  bool hasCalleeCondition(Function *callee) const {
    return callee &&
           callee_conditions_.find(callee) != callee_conditions_.end();
  }
  ConditionRef getCalleeCondition(Function *callee) const {
    auto it = callee_conditions_.find(callee);
    return it == callee_conditions_.end() ? ConditionRef::none() : it->second;
  }
  GuardedValueFlowRegionNode *getCalleeConditionRegion(Function *callee) const {
    auto it = callee_condition_regions_.find(callee);
    return it == callee_condition_regions_.end() ? nullptr : it->second;
  }

  GuardedValueFlowNode *getPseudoInput(Function *callee, unsigned idx) const;
  GuardedValueFlowNode *getPseudoOutput(Function *callee, unsigned idx) const;
  unsigned getNumPseudoInputs(Function *callee) const;
  unsigned getNumPseudoOutputs(Function *callee) const;

private:
  std::vector<Function *> callees_;
  std::vector<GuardedValueFlowNode *> common_inputs_;
  GuardedValueFlowNode *common_output_{nullptr};
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_inputs_;
  std::map<Function *, std::vector<GuardedValueFlowNode *>> pseudo_outputs_;
  std::map<std::pair<Function *, unsigned>, GuardedValueFlowNode *>
      input_summary_nodes_;
  std::map<std::pair<Function *, unsigned>, GuardedValueFlowNode *>
      output_summary_nodes_;
  std::set<Function *> back_edge_callees_;
  std::map<Function *, ConditionRef> callee_conditions_;
  std::map<Function *, GuardedValueFlowRegionNode *> callee_condition_regions_;
};

class GuardedValueFlowDereferenceSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowDereferenceSite(GuardedValueFlowGraph *graph,
                                  Instruction *inst)
      : GuardedValueFlowSite(Kind::DereferenceSite, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) {
    pointer_operand_ = node;
  }
  void setValueOperand(GuardedValueFlowNode *node) { value_operand_ = node; }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  GuardedValueFlowNode *getValueOperand() const { return value_operand_; }

private:
  // For loads only the pointer operand is populated. For stores both pointer
  // and value are recorded.
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *value_operand_{nullptr};
};

class GuardedValueFlowReturnSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowReturnSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::ReturnSite, graph, inst) {}
};

class GuardedValueFlowGEPReferenceSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowGEPReferenceSite(GuardedValueFlowGraph *graph,
                                   Instruction *inst)
      : GuardedValueFlowSite(Kind::GEP, graph, inst) {}

  void setPointerOperand(GuardedValueFlowNode *node) {
    pointer_operand_ = node;
  }
  GuardedValueFlowNode *getPointerOperand() const { return pointer_operand_; }
  void addOffsetOperand(GuardedValueFlowNode *node) {
    offset_operands_.push_back(node);
  }
  ArrayRef<GuardedValueFlowNode *> getOffsetOperands() const {
    return offset_operands_;
  }
  void setResultNode(GuardedValueFlowNode *node) { result_node_ = node; }
  GuardedValueFlowNode *getResultNode() const { return result_node_; }

private:
  // Offset operands stay aligned with the original IR indices even when the
  // lowering inserts temporary cast/add nodes internally.
  GuardedValueFlowNode *pointer_operand_{nullptr};
  GuardedValueFlowNode *result_node_{nullptr};
  std::vector<GuardedValueFlowNode *> offset_operands_;
};

class GuardedValueFlowCompareSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowCompareSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Compare, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

class GuardedValueFlowDivSite : public GuardedValueFlowSite {
public:
  GuardedValueFlowDivSite(GuardedValueFlowGraph *graph, Instruction *inst)
      : GuardedValueFlowSite(Kind::Div, graph, inst) {}

  void setLhsOperand(GuardedValueFlowNode *node) { lhs_operand_ = node; }
  void setRhsOperand(GuardedValueFlowNode *node) { rhs_operand_ = node; }
  GuardedValueFlowNode *getLhsOperand() const { return lhs_operand_; }
  GuardedValueFlowNode *getRhsOperand() const { return rhs_operand_; }

private:
  GuardedValueFlowNode *lhs_operand_{nullptr};
  GuardedValueFlowNode *rhs_operand_{nullptr};
};

} // namespace gvfg
} // namespace lotus
