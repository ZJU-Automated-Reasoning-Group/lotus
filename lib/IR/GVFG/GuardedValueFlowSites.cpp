#include "IR/GVFG/GuardedValueFlowSites.h"

#include "IR/GVFG/GuardedValueFlowGraph.h"

using namespace llvm;
using namespace lotus::gvfg;

void GuardedValueFlowCallSite::addCommonInput(GuardedValueFlowNode *node) {
  common_inputs_.push_back(node);
  if (node)
    node->addUseSite(this);
}

void GuardedValueFlowCallSite::addPseudoInput(Function *callee,
                                              GuardedValueFlowNode *node) {
  auto &inputs = pseudo_inputs_[callee];
  if (node) {
    node->setIndex(static_cast<unsigned>(inputs.size()));
    node->addUseSite(this);
  }
  inputs.push_back(node);
}

void GuardedValueFlowCallSite::addPseudoOutput(Function *callee,
                                               GuardedValueFlowNode *node) {
  auto &outputs = pseudo_outputs_[callee];
  if (node)
    node->setIndex(static_cast<unsigned>(outputs.size()));
  outputs.push_back(node);
}

void GuardedValueFlowCallSite::setCalleeCondition(
    Function *callee, ConditionRef condition,
    GuardedValueFlowRegionNode *region) {
  if (!callee)
    return;
  callee_conditions_[callee] = condition;
  if (region)
    callee_condition_regions_[callee] = region;
}

GuardedValueFlowNode *
GuardedValueFlowCallSite::getPseudoInput(Function *callee, unsigned idx) const {
  auto it = pseudo_inputs_.find(callee);
  if (it != pseudo_inputs_.end() && idx < it->second.size())
    return it->second[idx];
  return nullptr;
}

GuardedValueFlowNode *
GuardedValueFlowCallSite::getPseudoOutput(Function *callee,
                                          unsigned idx) const {
  auto it = pseudo_outputs_.find(callee);
  if (it != pseudo_outputs_.end() && idx < it->second.size())
    return it->second[idx];
  return nullptr;
}

unsigned GuardedValueFlowCallSite::getNumPseudoInputs(Function *callee) const {
  auto it = pseudo_inputs_.find(callee);
  return it == pseudo_inputs_.end() ? 0u
                                    : static_cast<unsigned>(it->second.size());
}

unsigned GuardedValueFlowCallSite::getNumPseudoOutputs(Function *callee) const {
  auto it = pseudo_outputs_.find(callee);
  return it == pseudo_outputs_.end() ? 0u
                                     : static_cast<unsigned>(it->second.size());
}
