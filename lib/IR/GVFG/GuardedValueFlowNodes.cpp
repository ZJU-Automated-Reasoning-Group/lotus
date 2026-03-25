#include "IR/GVFG/GuardedValueFlowNodes.h"

#include "IR/GVFG/GuardedValueFlowSites.h"

#include <algorithm>

using namespace llvm;
using namespace lotus::gvfg;

namespace {

static bool
isArithmeticFlowOpcode(GuardedValueFlowOpcodeNode::OpcodeKind opcode_kind) {
  switch (opcode_kind) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::URem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SRem:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FDiv:
  case GuardedValueFlowOpcodeNode::OpcodeKind::And:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Or:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Xor:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Shl:
  case GuardedValueFlowOpcodeNode::OpcodeKind::LShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AShr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Mul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FMul:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FAdd:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FSub:
    return true;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Concat:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Invalid:
  case GuardedValueFlowOpcodeNode::OpcodeKind::AddrSpaceCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::IntToPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::PtrToInt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::BitCast:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ZExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Trunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPTrunc:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPExt:
  case GuardedValueFlowOpcodeNode::OpcodeKind::SIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToSI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::UIToFP:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FPToUI:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ExtractElement:
  case GuardedValueFlowOpcodeNode::OpcodeKind::InsertElement:
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
  case GuardedValueFlowOpcodeNode::OpcodeKind::ICmp:
  case GuardedValueFlowOpcodeNode::OpcodeKind::FCmp:
    return false;
  }
  return false;
}

static bool isValueFlowParent(const GuardedValueFlowNode *node,
                              const GuardedValueFlowNode *parent,
                              bool enable_arithmetic_flow) {
  if (!node || !parent)
    return false;
  if (isa<GuardedValueFlowRegionNode>(parent))
    return false;

  auto *opcode_parent = dyn_cast<GuardedValueFlowOpcodeNode>(parent);
  if (!opcode_parent)
    return true;

  if (opcode_parent->getKind() == GuardedValueFlowNode::Kind::CastOpcode)
    return true;

  switch (opcode_parent->getOpcodeKind()) {
  case GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Add:
  case GuardedValueFlowOpcodeNode::OpcodeKind::Sub:
    return true;
  case GuardedValueFlowOpcodeNode::OpcodeKind::Select:
    return !opcode_parent->children().empty() &&
           opcode_parent->children().front().target != node;
  default:
    return enable_arithmetic_flow &&
           isArithmeticFlowOpcode(opcode_parent->getOpcodeKind());
  }
}

} // namespace

GuardedValueFlowNode::GuardedValueFlowNode(Kind kind, Type *type,
                                           GuardedValueFlowGraph *graph,
                                           BasicBlock *block, Value *llvm_value,
                                           Instruction *dbg_inst)
    : kind_(kind), type_(type), graph_(graph), block_(block),
      llvm_value_(llvm_value), dbg_inst_(dbg_inst) {}

void GuardedValueFlowNode::addChild(GuardedValueFlowNode *child,
                                    float confidence, ConditionRef condition) {
  if (!child)
    return;
  children_.push_back({child, confidence, condition});
  auto parent_it =
      std::find_if(child->parents_.begin(), child->parents_.end(),
                   [&](const Edge &edge) { return edge.target == this; });
  if (parent_it != child->parents_.end()) {
    parent_it->confidence = confidence;
    parent_it->condition = condition;
    return;
  }
  child->parents_.push_back({this, confidence, condition});
}

void GuardedValueFlowNode::clearChildren() {
  for (const Edge &edge : children_) {
    if (!edge.target)
      continue;
    auto &parents = edge.target->parents_;
    parents.erase(std::remove_if(parents.begin(), parents.end(),
                                 [&](const Edge &parent_edge) {
                                   return parent_edge.target == this;
                                 }),
                  parents.end());
  }
  children_.clear();
}

bool GuardedValueFlowNode::containsParent(
    const GuardedValueFlowNode *parent) const {
  return std::any_of(parents_.begin(), parents_.end(),
                     [&](const Edge &edge) { return edge.target == parent; });
}

std::vector<GuardedValueFlowNode *>
GuardedValueFlowNode::getValueFlowParents(bool enable_arithmetic_flow) const {
  std::vector<GuardedValueFlowNode *> result;
  for (const Edge &edge : parents_) {
    if (isValueFlowParent(this, edge.target, enable_arithmetic_flow))
      result.push_back(edge.target);
  }
  return result;
}

void GuardedValueFlowNode::addUseSite(GuardedValueFlowSite *site) {
  for (GuardedValueFlowSite *existing : use_sites_) {
    if (existing == site)
      return;
  }
  use_sites_.push_back(site);
}

void GuardedValueFlowNode::addMatchingRegion(GuardedValueFlowNode *producer,
                                             GuardedValueFlowRegionNode *region,
                                             ConditionRef provenance) {
  for (auto &existing : matching_regions_) {
    if (existing.producer == producer) {
      existing.region = region;
      existing.provenance = provenance;
      return;
    }
  }
  matching_regions_.push_back({producer, region, provenance});
}

GuardedValueFlowRegionNode *GuardedValueFlowNode::getMatchingRegion(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.region;
  }
  return nullptr;
}

ConditionRef GuardedValueFlowNode::getMatchingCondition(
    const GuardedValueFlowNode *producer) const {
  for (const auto &entry : matching_regions_) {
    if (entry.producer == producer)
      return entry.provenance;
  }
  return ConditionRef::none();
}

void GuardedValueFlowPhiNode::addIncoming(GuardedValueFlowNode *value_node,
                                          BasicBlock *incoming_block,
                                          GuardedValueFlowNode *condition_node,
                                          bool condition_sense,
                                          ConditionRef condition) {
  incoming_.push_back(
      {value_node, incoming_block, condition_node, condition_sense, condition});
  addChild(value_node, 1.0f, condition);
}

void GuardedValueFlowReturnNode::addReturnValueSitePair(
    GuardedValueFlowNode *value_node, GuardedValueFlowReturnSite *site) {
  return_sites_[value_node] = site;
  if (site)
    addUseSite(site);
}

GuardedValueFlowReturnSite *GuardedValueFlowReturnNode::getReturnSite(
    const GuardedValueFlowNode *value_node) const {
  auto it = return_sites_.find(value_node);
  return it == return_sites_.end() ? nullptr : it->second;
}
