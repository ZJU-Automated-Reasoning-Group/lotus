#pragma once

#include "IR/GVFG/ConditionRef.h"

#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace lotus {
namespace gvfg {

using llvm::ArrayRef;
using llvm::BasicBlock;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::Function;
using llvm::Instruction;
using llvm::path_cond_t;
using llvm::Type;
using llvm::Value;

class GuardedValueFlowGraph;
class GuardedValueFlowSite;
class GuardedValueFlowReturnSite;
class GuardedValueFlowRegionNode;

// AccessPath records the abstract field path attached to interface nodes and
// summary nodes. Offsets are stored from leaf to root so the adapter can append
// newly discovered outer segments without rebuilding the whole path.
class AccessPath {
public:
  AccessPath() = default;
  AccessPath(Value *base, int64_t offset, bool is_from_return = false) {
    addLevel(base, offset, is_from_return);
  }

  Value *getBase() const { return base_; }
  int getDepth() const { return static_cast<int>(offsets_reversed_.size()); }
  int64_t getOffset(int idx) const {
    assert(idx >= 0 && idx < getDepth() && "Invalid access path offset index");
    return offsets_reversed_[offsets_reversed_.size() - idx - 1];
  }
  bool isFromReturn() const { return is_from_return_; }
  bool empty() const { return base_ == nullptr && offsets_reversed_.empty(); }

  void addLevel(Value *base, int64_t offset, bool is_from_return = false) {
    base_ = base;
    offsets_reversed_.push_back(offset);
    is_from_return_ = is_from_return;
  }

  void reset(Value *base = nullptr, bool is_from_return = false) {
    offsets_reversed_.clear();
    base_ = base;
    is_from_return_ = is_from_return;
  }

  void resetCurrentLevel(Value *base, int64_t offset,
                         bool is_from_return = false) {
    if (!offsets_reversed_.empty()) {
      base_ = base;
      offsets_reversed_.back() -= offset;
      is_from_return_ = is_from_return;
    }
  }

  void reset(const AccessPath &other) {
    offsets_reversed_ = other.offsets_reversed_;
    base_ = other.base_;
    is_from_return_ = other.is_from_return_;
  }

private:
  Value *base_{nullptr};
  std::vector<int64_t> offsets_reversed_;
  bool is_from_return_{false};
};

// GuardedValueFlowNode is the common node type for both SSA values and
// structural helper nodes. Edges are directed from a result/consumer to the
// value, memory node, or expression node it depends on.
class GuardedValueFlowNode {
public:
  enum class Kind {
    CommonArgument,
    PseudoArgument,
    VariableArgument,
    CommonReturn,
    PseudoReturn,
    SimpleOperand,
    UndefValue,
    LoadMemory,
    StoreMemory,
    Phi,
    Region,
    CallSiteCommonOutput,
    CallSitePseudoOutput,
    CallSitePseudoInput,
    CallSiteArgumentSummary,
    CallSiteReturnSummary,
    InterfaceCondition,
    SimpleOpcode,
    CastOpcode,
    Unknown,
  };

  struct Edge {
    GuardedValueFlowNode *target{nullptr};
    float confidence{1.0f};
    // Non-empty when the dependency only holds under a structural or imported
    // path condition.
    ConditionRef condition;
  };

  struct MatchingRegion {
    // `producer` is the node reachable from a load-memory node, while `region`
    // records the path condition under which that producer is valid.
    GuardedValueFlowNode *producer{nullptr};
    GuardedValueFlowRegionNode *region{nullptr};
    ConditionRef provenance;
  };

  GuardedValueFlowNode(Kind kind, Type *type, GuardedValueFlowGraph *graph,
                       BasicBlock *block, Value *llvm_value = nullptr,
                       Instruction *dbg_inst = nullptr);
  virtual ~GuardedValueFlowNode() = default;

  Kind getKind() const { return kind_; }
  Type *getType() const { return type_; }
  GuardedValueFlowGraph *getGraph() const { return graph_; }
  BasicBlock *getParentBasicBlock() const { return block_; }
  Value *getLLVMValue() const { return llvm_value_; }
  Instruction *getDebugInstruction() const { return dbg_inst_; }
  unsigned getNodeId() const { return node_id_; }

  void addChild(GuardedValueFlowNode *child, float confidence = 1.0f,
                ConditionRef condition = ConditionRef::none());
  void clearChildren();
  ArrayRef<Edge> children() const { return children_; }
  ArrayRef<Edge> parents() const { return parents_; }
  unsigned getNumParents() const {
    return static_cast<unsigned>(parents_.size());
  }
  bool containsParent(const GuardedValueFlowNode *parent) const;
  std::vector<GuardedValueFlowNode *>
  getValueFlowParents(bool enable_arithmetic_flow = false) const;

  void addUseSite(GuardedValueFlowSite *site);
  ArrayRef<GuardedValueFlowSite *> useSites() const { return use_sites_; }

  GuardedValueFlowRegionNode *getRegion() const { return region_; }

  void setDescription(std::string desc) { description_ = std::move(desc); }
  const std::string &getDescription() const { return description_; }

  void setAccessPath(AccessPath path) { access_path_ = path; }
  AccessPath &getAccessPath() { return access_path_; }
  const AccessPath &getAccessPath() const { return access_path_; }

  void setIndex(unsigned idx) { index_ = idx; }
  unsigned getIndex() const { return index_; }

  void addMatchingRegion(GuardedValueFlowNode *producer,
                         GuardedValueFlowRegionNode *region,
                         ConditionRef provenance = ConditionRef::none());
  GuardedValueFlowRegionNode *
  getMatchingRegion(const GuardedValueFlowNode *producer) const;
  ConditionRef getMatchingCondition(const GuardedValueFlowNode *producer) const;
  void clearMatchingRegions() { matching_regions_.clear(); }
  ArrayRef<MatchingRegion> getMatchingRegions() const {
    return matching_regions_;
  }

protected:
  Kind kind_;
  Type *type_;
  GuardedValueFlowGraph *graph_;
  BasicBlock *block_;
  Value *llvm_value_;
  Instruction *dbg_inst_;
  unsigned node_id_{0};
  unsigned index_{0};
  std::string description_;
  AccessPath access_path_;
  GuardedValueFlowRegionNode *region_{nullptr};
  std::vector<Edge> children_;
  std::vector<Edge> parents_;
  std::vector<GuardedValueFlowSite *> use_sites_;
  std::vector<MatchingRegion> matching_regions_;

  friend class GuardedValueFlowGraph;
};

class GuardedValueFlowArgumentNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowArgumentNode(Kind kind, Type *type,
                               GuardedValueFlowGraph *graph, BasicBlock *block,
                               Value *llvm_value)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast<Instruction>(llvm_value)) {}

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonArgument ||
           node->getKind() == Kind::PseudoArgument ||
           node->getKind() == Kind::VariableArgument;
  }
};

// Region nodes summarize control/path conditions for blocks, imported path
// facts, and composed boolean guards. Non-region nodes inherit the region of
// their parent block unless the adapter later places them elsewhere.
class GuardedValueFlowRegionNode : public GuardedValueFlowNode {
public:
  struct ConstraintState {
    std::map<const GuardedValueFlowNode *, bool> assignments;
  };

  enum class Form {
    AlwaysTrue,
    AlwaysFalse,
    Unit,
    Semantic,
    ImportedInterface,
    And,
    Or,
    Not,
  };

  GuardedValueFlowRegionNode(Type *type, GuardedValueFlowGraph *graph,
                             BasicBlock *block, Form form,
                             GuardedValueFlowNode *condition_node,
                             bool condition_sense, ConditionRef condition)
      : GuardedValueFlowNode(Kind::Region, type, graph, block, nullptr,
                             block ? block->getTerminator() : nullptr),
        form_(form), condition_node_(condition_node),
        condition_sense_(condition_sense), region_condition_(condition) {
    if (form_ == Form::AlwaysTrue) {
      setAlwaysTrue();
      return;
    }
    if (form_ == Form::AlwaysFalse) {
      setAlwaysFalse();
      return;
    }

    if (form_ == Form::Unit) {
      if (!condition_node_) {
        if (condition_sense_)
          setAlwaysTrue();
        else
          setAlwaysFalse();
        return;
      }

      if (auto *condition_region =
              dyn_cast<GuardedValueFlowRegionNode>(condition_node_)) {
        if (condition_region->isAlwaysTrue()) {
          if (condition_sense_)
            setAlwaysTrue();
          else
            setAlwaysFalse();
          return;
        }
        if (condition_region->isAlwaysFalse()) {
          if (condition_sense_)
            setAlwaysFalse();
          else
            setAlwaysTrue();
          return;
        }
      }

      constraint_state_.assignments[this] = true;
      constraint_state_.assignments[condition_node_] = condition_sense_;
      is_satisfiable_ = true;
    }
  }

  Form getForm() const { return form_; }
  bool isAlwaysTrue() const { return form_ == Form::AlwaysTrue; }
  bool isAlwaysFalse() const { return form_ == Form::AlwaysFalse; }
  bool isInterfaceRegion() const { return form_ == Form::ImportedInterface; }
  bool isSemantic() const {
    return form_ == Form::Semantic || form_ == Form::ImportedInterface;
  }
  bool isLocalSemanticRegion() const { return form_ == Form::Semantic; }
  bool isImportedSemanticRegion() const {
    return form_ == Form::ImportedInterface;
  }
  bool isCompound() const {
    return form_ == Form::And || form_ == Form::Or || form_ == Form::Not;
  }
  GuardedValueFlowNode *getConditionNode() const { return condition_node_; }
  bool getConditionSense() const { return condition_sense_; }
  const ConditionRef &getRegionCondition() const { return region_condition_; }
  Function *getInterfaceOwnerFunction() const {
    return interface_owner_function_;
  }
  Function *getInterfaceOriginFunction() const {
    return interface_origin_function_;
  }
  path_cond_t getInterfacePathCondition() const {
    return interface_path_condition_;
  }
  path_cond_t getImportedSourceCondition() const {
    return imported_source_condition_;
  }
  bool isSatisfiable() const { return is_satisfiable_; }
  const ConstraintState &getConstraintState() const {
    return constraint_state_;
  }
  void setInterfaceMetadata(Function *owner_function, Function *origin_function,
                            path_cond_t interface_path_condition,
                            path_cond_t imported_source_condition) {
    interface_owner_function_ = owner_function;
    interface_origin_function_ = origin_function;
    interface_path_condition_ = interface_path_condition;
    imported_source_condition_ = imported_source_condition;
  }

private:
  Form form_;
  GuardedValueFlowNode *condition_node_{nullptr};
  bool condition_sense_{true};
  ConditionRef region_condition_;
  bool is_satisfiable_{true};
  ConstraintState constraint_state_;
  Function *interface_owner_function_{nullptr};
  Function *interface_origin_function_{nullptr};
  path_cond_t interface_path_condition_{nullptr};
  path_cond_t imported_source_condition_{nullptr};

public:
  void setAlwaysFalse() {
    form_ = Form::AlwaysFalse;
    is_satisfiable_ = false;
    constraint_state_.assignments.clear();
  }

  void setAlwaysTrue() {
    form_ = Form::AlwaysTrue;
    is_satisfiable_ = true;
    constraint_state_.assignments.clear();
  }

  void setConstraintState(ConstraintState state) {
    constraint_state_ = std::move(state);
    is_satisfiable_ = true;
  }

  void markUnsatisfiable() {
    is_satisfiable_ = false;
    constraint_state_.assignments.clear();
  }

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Region;
  }
};

class GuardedValueFlowOpcodeNode : public GuardedValueFlowNode {
public:
  enum class OpcodeKind {
    Invalid,
    URem,
    FRem,
    SRem,
    UDiv,
    SDiv,
    FDiv,
    And,
    Or,
    Xor,
    Shl,
    LShr,
    AShr,
    Mul,
    FMul,
    FAdd,
    FSub,
    Add,
    Sub,
    AddrSpaceCast,
    IntToPtr,
    PtrToInt,
    BitCast,
    ZExt,
    SExt,
    Trunc,
    FPTrunc,
    FPExt,
    SIToFP,
    FPToSI,
    UIToFP,
    FPToUI,
    ExtractElement,
    InsertElement,
    GetElementPtr,
    Select,
    ICmp,
    FCmp,
    Concat,
  };

  GuardedValueFlowOpcodeNode(Kind kind, Type *type,
                             GuardedValueFlowGraph *graph, BasicBlock *block,
                             OpcodeKind opcode_kind)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, nullptr),
        opcode_kind_(opcode_kind) {}

  OpcodeKind getOpcodeKind() const { return opcode_kind_; }
  void setCmpPredicate(int predicate) { cmp_predicate_ = predicate; }
  int getCmpPredicate() const { return cmp_predicate_; }
  void setCastWidths(uint64_t src_bits, uint64_t dst_bits) {
    cast_src_bits_ = src_bits;
    cast_dst_bits_ = dst_bits;
  }
  uint64_t getCastSrcBits() const { return cast_src_bits_; }
  uint64_t getCastDstBits() const { return cast_dst_bits_; }
  void setIntConstant(int64_t value) {
    has_int_constant_ = true;
    int_constant_ = value;
  }
  bool hasIntConstant() const { return has_int_constant_; }
  int64_t getIntConstant() const { return int_constant_; }

private:
  OpcodeKind opcode_kind_;
  int cmp_predicate_{-1};
  uint64_t cast_src_bits_{0};
  uint64_t cast_dst_bits_{0};
  bool has_int_constant_{false};
  int64_t int_constant_{0};

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::SimpleOpcode ||
           node->getKind() == Kind::CastOpcode;
  }
};

class GuardedValueFlowPhiNode : public GuardedValueFlowNode {
public:
  struct Incoming {
    GuardedValueFlowNode *value_node{nullptr};
    BasicBlock *incoming_block{nullptr};
    // Immediate edge-local guard for this incoming value. This is narrower than
    // the enclosing block region and is what downstream path-sensitive code
    // should consult first for PHI semantics.
    GuardedValueFlowNode *condition_node{nullptr};
    bool condition_sense{true};
    ConditionRef condition;
  };

  GuardedValueFlowPhiNode(Type *type, GuardedValueFlowGraph *graph,
                          BasicBlock *block, Value *llvm_value,
                          Instruction *dbg_inst)
      : GuardedValueFlowNode(Kind::Phi, type, graph, block, llvm_value,
                             dbg_inst) {}

  void addIncoming(GuardedValueFlowNode *value_node, BasicBlock *incoming_block,
                   GuardedValueFlowNode *condition_node, bool condition_sense,
                   ConditionRef condition);
  ArrayRef<Incoming> incoming() const { return incoming_; }

private:
  std::vector<Incoming> incoming_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::Phi;
  }
};

class GuardedValueFlowReturnNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowReturnNode(Kind kind, Type *type,
                             GuardedValueFlowGraph *graph, BasicBlock *block,
                             Value *llvm_value = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value,
                             dyn_cast_or_null<Instruction>(llvm_value)) {}

  void addReturnValueSitePair(GuardedValueFlowNode *value_node,
                              GuardedValueFlowReturnSite *site);
  GuardedValueFlowReturnSite *
  getReturnSite(const GuardedValueFlowNode *value_node) const;

private:
  std::map<const GuardedValueFlowNode *, GuardedValueFlowReturnSite *>
      return_sites_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CommonReturn ||
           node->getKind() == Kind::PseudoReturn;
  }
};

// Call output nodes cover three interface roles:
// - CommonOutput: direct non-void call result
// - PseudoInput: per-callee incoming side-effect channel at a callsite
// - PseudoOutput: per-callee outgoing side-effect channel at a callsite
class GuardedValueFlowCallOutputNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallOutputNode(Kind kind, Type *type,
                                 GuardedValueFlowGraph *graph,
                                 BasicBlock *block, Value *llvm_value,
                                 Instruction *call_site,
                                 Function *callee = nullptr)
      : GuardedValueFlowNode(kind, type, graph, block, llvm_value, call_site),
        call_site_(call_site), callee_(callee) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }

private:
  Instruction *call_site_;
  Function *callee_;

public:
  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteCommonOutput ||
           node->getKind() == Kind::CallSitePseudoOutput ||
           node->getKind() == Kind::CallSitePseudoInput;
  }
};

// Summary nodes model access-path buckets that are intentionally coarser than
// direct pseudo interfaces. They remain separate so callers can distinguish
// exact interface channels from summary-only channels.
class GuardedValueFlowCallSummaryNode : public GuardedValueFlowNode {
public:
  GuardedValueFlowCallSummaryNode(Kind kind, Type *type,
                                  GuardedValueFlowGraph *graph,
                                  BasicBlock *block, Instruction *call_site,
                                  Function *callee, unsigned summary_index)
      : GuardedValueFlowNode(kind, type, graph, block, nullptr, call_site),
        call_site_(call_site), callee_(callee), summary_index_(summary_index) {}

  Instruction *getCallSite() const { return call_site_; }
  Function *getCallee() const { return callee_; }
  unsigned getSummaryIndex() const { return summary_index_; }

  static bool classof(const GuardedValueFlowNode *node) {
    return node->getKind() == Kind::CallSiteArgumentSummary ||
           node->getKind() == Kind::CallSiteReturnSummary;
  }

private:
  Instruction *call_site_;
  Function *callee_;
  unsigned summary_index_{0};
};

} // namespace gvfg
} // namespace lotus
