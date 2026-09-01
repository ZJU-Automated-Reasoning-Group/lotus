#include "CFL/Classical/LLVMAliasAnalysis.h"

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/SolverBase.h"
#include "CFL/Classical/AserConstraintAdapter.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

namespace lotus::cfl::classical {
namespace {

using MemoryModel = aser::FSMemModel<aser::NoCtx>;
using LanguageModel =
    aser::DefaultLangModel<aser::NoCtx, MemoryModel, aser::BitVectorPTS>;

class ConstraintBuilder final
    : public aser::SolverBase<LanguageModel, ConstraintBuilder> {
public:
  void solve() {}
};

struct LLVMGepOffsetResolver {
  const llvm::DataLayout *layout = nullptr;

  std::optional<std::uint32_t>
  operator()(const aser::CGNodeBase<aser::NoCtx> &,
             const aser::CGNodeBase<aser::NoCtx> &target) const {
    const auto *pointer = llvm::dyn_cast<aser::CGPtrNode<aser::NoCtx>>(&target);
    if (!pointer || pointer->isAnonNode() || !layout) {
      return std::nullopt;
    }
    const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
        pointer->getPointer()->getValue());
    if (!gep) {
      return std::nullopt;
    }
    llvm::APInt offset(layout->getIndexTypeSizeInBits(gep->getType()), 0);
    if (!gep->accumulateConstantOffset(*layout, offset) ||
        offset.isNegative() || offset.getActiveBits() > 32) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(offset.getZExtValue());
  }
};

using Synchronizer = AserAliasSynchronizer<aser::NoCtx, LLVMGepOffsetResolver>;
using ObjectNode = aser::CGObjNode<MemoryModel>;

} // namespace

class LLVMCFLAliasAnalysis::Impl {
public:
  explicit Impl(LLVMAliasOptions options) : options_(std::move(options)) {}

  ReachabilityStats analyze(llvm::Module &module) {
    if (analyzed_) {
      throw std::logic_error("LLVMCFLAliasAnalysis instances are single-use");
    }
    analyzed_ = true;
    module_ = &module;
    builder_ = std::make_unique<ConstraintBuilder>();
    builder_->analyze(&module, options_.entry);

    auto *graph = builder_->getConsGraph();
    LLVMGepOffsetResolver resolver{&module.getDataLayout()};
    client_ = std::make_unique<AliasClient>(
        makeAliasClient(*graph, resolver, options_.encoding));
    synchronizer_ = std::make_unique<Synchronizer>(*graph, *client_, resolver);
    buildValueIndex();
    addSupplementalConstraints();

    statistics_ = client_->solveToFixedPoint(
        options_.backend,
        [&](AliasClient &) {
          std::vector<aser::NodeID> function_pointers;
          for (auto *node : *graph) {
            if (node->isFunctionPtr()) {
              function_pointers.push_back(node->getNodeID());
            }
          }
          const bool calls_changed =
              builder_->getLangModelForClients()->resolveFunctionPointers(
                  function_pointers, [&](aser::NodeID pointer) {
                    std::vector<aser::NodeID> result;
                    for (std::size_t mapped : client_->pointsTo(
                             synchronizer_->mappedNode(pointer))) {
                      if (auto source = synchronizer_->sourceNode(mapped)) {
                        result.push_back(*source);
                      }
                    }
                    return result;
                  });
          const bool constraints_changed = synchronizer_->synchronize();
          return calls_changed || constraints_changed;
        },
        options_.max_callgraph_rounds);
    buildValueIndex();
    return statistics_;
  }

  std::optional<std::size_t> nodeForValue(const llvm::Value *value) const {
    requireAnalysis();
    const auto it = value_nodes_.find(value);
    if (it != value_nodes_.end()) {
      return it->second;
    }
    if (value && value->getType()->isPointerTy()) {
      const llvm::Value *stripped = value->stripPointerCasts();
      if (const auto stripped_it = value_nodes_.find(stripped);
          stripped_it != value_nodes_.end()) {
        return stripped_it->second;
      }
    }
    return std::nullopt;
  }

  bool mayAlias(const llvm::Value *lhs, const llvm::Value *rhs) const {
    const auto lhs_node = nodeForValue(lhs);
    const auto rhs_node = nodeForValue(rhs);
    return lhs_node && rhs_node && client_->mayAlias(*lhs_node, *rhs_node);
  }

  std::vector<const llvm::Value *> pointsTo(const llvm::Value *pointer) const {
    const auto node = nodeForValue(pointer);
    if (!node) {
      return {};
    }
    std::set<const llvm::Value *> values;
    auto *graph = builder_->getConsGraph();
    for (std::size_t mapped : client_->pointsTo(*node)) {
      const auto source = synchronizer_->sourceNode(mapped);
      if (!source || *source >= graph->getNodeNum()) {
        continue;
      }
      if (const auto *object = llvm::dyn_cast<ObjectNode>((*graph)[*source])) {
        values.insert(object->getObject()->getValue());
      }
    }
    return {values.begin(), values.end()};
  }

  const AliasClient &client() const {
    requireAnalysis();
    return *client_;
  }

  const ReachabilityStats &statistics() const {
    requireAnalysis();
    return statistics_;
  }

private:
  void requireAnalysis() const {
    if (!analyzed_ || !client_) {
      throw std::logic_error("analyze() has not been called");
    }
  }

  void buildValueIndex() {
    value_nodes_.clear();
    for (const auto *node : *builder_->getConsGraph()) {
      const auto *pointer = llvm::dyn_cast<aser::CGPtrNode<aser::NoCtx>>(node);
      if (!pointer || pointer->isAnonNode()) {
        continue;
      }
      value_nodes_.emplace(pointer->getPointer()->getValue(),
                           synchronizer_->mappedNode(node->getNodeID()));
    }
  }

  std::optional<std::size_t> mappedValue(const llvm::Value *value) const {
    if (!value) {
      return std::nullopt;
    }
    if (const auto it = value_nodes_.find(value); it != value_nodes_.end()) {
      return it->second;
    }
    if (value->getType()->isPointerTy()) {
      if (const auto it = value_nodes_.find(value->stripPointerCasts());
          it != value_nodes_.end()) {
        return it->second;
      }
    }
    return std::nullopt;
  }

  void addSupplementalConstraints() {
    for (llvm::GlobalVariable &global : module_->globals()) {
      if (!global.hasInitializer() ||
          !global.getInitializer()->getType()->isPointerTy() ||
          global.getInitializer()->isNullValue()) {
        continue;
      }
      const auto source = mappedValue(global.getInitializer());
      const auto target = mappedValue(&global);
      if (source && target) {
        client_->addConstraint(*source, *target,
                               AliasConstraintEdgeKind::Store);
      }
    }

    for (llvm::Function &function : *module_) {
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        const auto *copy = llvm::dyn_cast<llvm::MemCpyInst>(&instruction);
        if (!copy) {
          continue;
        }
        const auto source = mappedValue(copy->getSource());
        const auto target = mappedValue(copy->getDest());
        if (!source || !target) {
          continue;
        }
        const std::size_t temporary = client_->addNode(
            "memcpy_" + std::to_string(client_->graph().vertexCount()));
        client_->addConstraint(*source, temporary,
                               AliasConstraintEdgeKind::Load);
        client_->addConstraint(temporary, *target,
                               AliasConstraintEdgeKind::Store);
      }
    }
  }

  LLVMAliasOptions options_;
  llvm::Module *module_ = nullptr;
  bool analyzed_ = false;
  std::unique_ptr<ConstraintBuilder> builder_;
  std::unique_ptr<AliasClient> client_;
  std::unique_ptr<Synchronizer> synchronizer_;
  std::unordered_map<const llvm::Value *, std::size_t> value_nodes_;
  ReachabilityStats statistics_;
};

LLVMCFLAliasAnalysis::LLVMCFLAliasAnalysis(LLVMAliasOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LLVMCFLAliasAnalysis::~LLVMCFLAliasAnalysis() = default;
LLVMCFLAliasAnalysis::LLVMCFLAliasAnalysis(LLVMCFLAliasAnalysis &&) noexcept =
    default;
LLVMCFLAliasAnalysis &
LLVMCFLAliasAnalysis::operator=(LLVMCFLAliasAnalysis &&) noexcept = default;

ReachabilityStats LLVMCFLAliasAnalysis::analyze(llvm::Module &module) {
  return impl_->analyze(module);
}

bool LLVMCFLAliasAnalysis::mayAlias(const llvm::Value *lhs,
                                    const llvm::Value *rhs) const {
  return impl_->mayAlias(lhs, rhs);
}

std::vector<const llvm::Value *>
LLVMCFLAliasAnalysis::pointsTo(const llvm::Value *pointer) const {
  return impl_->pointsTo(pointer);
}

std::optional<std::size_t>
LLVMCFLAliasAnalysis::nodeForValue(const llvm::Value *value) const {
  return impl_->nodeForValue(value);
}

const AliasClient &LLVMCFLAliasAnalysis::client() const {
  return impl_->client();
}

const ReachabilityStats &LLVMCFLAliasAnalysis::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical
