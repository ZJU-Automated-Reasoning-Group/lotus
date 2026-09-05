#include "CFL/Classical/Clients/Alias/LLVMAliasAnalysis.h"

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/LanguageModel/DefaultLangModel/DefaultLangModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Models/MemoryModel/FieldSensitive/FSMemModel.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/PointsTo/BitVectorPTS.h"
#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/SolverBase.h"
#include "CFL/Classical/Clients/Alias/AserConstraintAdapter.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <llvm/ADT/MapVector.h>
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

  AserGepResolution
  operator()(const aser::CGNodeBase<aser::NoCtx> &,
             const aser::CGNodeBase<aser::NoCtx> &target) const {
    const auto *pointer = llvm::dyn_cast<aser::CGPtrNode<aser::NoCtx>>(&target);
    if (!pointer || pointer->isAnonNode() || !layout) {
      return {true, 0, 1};
    }
    const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(
        pointer->getPointer()->getValue());
    if (!gep) {
      return {true, 0, 1};
    }
    const unsigned bit_width = layout->getIndexTypeSizeInBits(gep->getType());
    llvm::MapVector<llvm::Value *, llvm::APInt> variables;
    llvm::APInt constant(bit_width, 0);
    if (!gep->collectOffset(*layout, bit_width, variables, constant) ||
        constant.isNegative() || constant.getActiveBits() > 32) {
      return {true, 0, 1};
    }
    const std::uint32_t constant_offset =
        static_cast<std::uint32_t>(constant.getZExtValue());
    if (variables.empty()) {
      return {false, constant_offset, std::nullopt};
    }

    std::uint32_t modulus = 0;
    for (const auto &[_, coefficient] : variables) {
      if (coefficient.isNegative() || coefficient.getActiveBits() > 32) {
        return {true, 0, 1};
      }
      modulus = std::gcd(
          modulus, static_cast<std::uint32_t>(coefficient.getZExtValue()));
    }
    if (modulus == 0) {
      return {true, 0, 1};
    }
    return {true, constant_offset % modulus, modulus};
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
    const auto frontend_start = std::chrono::steady_clock::now();
    builder_ = std::make_unique<ConstraintBuilder>();
    builder_->analyze(&module, options_.entry);
    frontend_time_microseconds_ =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - frontend_start)
            .count();

    const auto initialization_start = std::chrono::steady_clock::now();
    auto *graph = builder_->getConsGraph();
    LLVMGepOffsetResolver resolver{&module.getDataLayout()};
    client_ = std::make_unique<AliasClient>(
        makeAliasClient(*graph, resolver, options_.encoding));
    synchronizer_ = std::make_unique<Synchronizer>(*graph, *client_, resolver);
    buildValueIndex();
    identifyUnknownValues();
    addSupplementalConstraints();
    client_initialization_microseconds_ =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - initialization_start)
            .count();

    auto discover_constraints = [&](AliasClient &) {
      const auto discovery_start = std::chrono::steady_clock::now();
      const bool unknown_changed = refineUnknownValues();
      std::vector<aser::NodeID> function_pointers;
      for (auto *node : *graph) {
        if (node->isFunctionPtr()) {
          function_pointers.push_back(node->getNodeID());
        }
      }
      std::vector<std::size_t> mapped_function_pointers;
      mapped_function_pointers.reserve(function_pointers.size());
      for (aser::NodeID pointer : function_pointers) {
        mapped_function_pointers.push_back(synchronizer_->mappedNode(pointer));
      }
      std::vector<std::pair<std::size_t, std::size_t>> function_addresses;
      for (const auto &[object, pointer] :
           client_->graph().edgesForLabel("addr")) {
        const auto source = synchronizer_->sourceNode(object);
        if (!source || *source >= graph->getNodeNum()) {
          continue;
        }
        const auto *object_node = llvm::dyn_cast<ObjectNode>((*graph)[*source]);
        if (object_node &&
            llvm::isa<llvm::Function>(object_node->getObject()->getValue())) {
          function_addresses.emplace_back(object, pointer);
        }
      }
      const auto address_objects = client_->matchingAddressTakenObjects(
          mapped_function_pointers, function_addresses);
      const bool calls_changed =
          builder_->getLangModelForClients()->resolveFunctionPointers(
              function_pointers, [&](aser::NodeID pointer) {
                std::vector<aser::NodeID> result;
                const auto *pointer_node =
                    llvm::dyn_cast<aser::CGPtrNode<aser::NoCtx>>(
                        (*graph)[pointer]);
                if (pointer_node && !pointer_node->isAnonNode() &&
                    isUnknownValue(pointer_node->getPointer()->getValue())) {
                  for (const auto &[object, _] : function_addresses) {
                    if (const auto source = synchronizer_->sourceNode(object)) {
                      result.push_back(*source);
                    }
                  }
                  return result;
                }
                const auto targets =
                    address_objects.find(synchronizer_->mappedNode(pointer));
                if (targets == address_objects.end()) {
                  return result;
                }
                for (std::size_t mapped : targets->second) {
                  if (auto source = synchronizer_->sourceNode(mapped)) {
                    result.push_back(*source);
                  }
                }
                return result;
              });
      const bool constraints_changed = synchronizer_->synchronize();
      if (constraints_changed) {
        buildValueIndex();
      }
      const bool supplemental_changed = addSupplementalConstraints();
      client_discovery_microseconds_ +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - discovery_start)
              .count();
      return unknown_changed || calls_changed || constraints_changed ||
             supplemental_changed;
    };
    if (options_.specialized_backend) {
      statistics_ = client_->solveToFixedPoint(
          *options_.specialized_backend, discover_constraints,
          options_.max_callgraph_rounds, options_.simplify_focr_cycles);
    } else {
      statistics_ =
          client_->solveToFixedPoint(options_.backend, discover_constraints,
                                     options_.max_callgraph_rounds);
    }
    statistics_.frontend_time_microseconds = frontend_time_microseconds_;
    statistics_.client_initialization_microseconds =
        client_initialization_microseconds_;
    statistics_.client_discovery_microseconds = client_discovery_microseconds_;
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
    if (isUnknownValue(lhs) || isUnknownValue(rhs)) {
      return true;
    }
    const auto lhs_node = nodeForValue(lhs);
    const auto rhs_node = nodeForValue(rhs);
    if (!lhs_node || !rhs_node) {
      return true;
    }
    return client_->mayAlias(*lhs_node, *rhs_node);
  }

  std::vector<const llvm::Value *> pointsTo(const llvm::Value *pointer) const {
    const auto node = nodeForValue(pointer);
    if (!node) {
      return {};
    }
    std::set<const llvm::Value *> values;
    auto *graph = builder_->getConsGraph();
    if (isUnknownValue(pointer)) {
      for (auto *candidate : *graph) {
        if (const auto *object = llvm::dyn_cast<ObjectNode>(candidate)) {
          values.insert(object->getObject()->getValue());
        }
      }
      return {values.begin(), values.end()};
    }
    for (std::size_t mapped : client_->pointsTo(*node)) {
      const auto base = client_->baseObject(mapped);
      const auto source = synchronizer_->sourceNode(base.value_or(mapped));
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

  bool isUnknownValue(const llvm::Value *value) const {
    if (!value) {
      return false;
    }
    if (unknown_values_.count(value) != 0) {
      return true;
    }
    return value->getType()->isPointerTy() &&
           unknown_values_.count(value->stripPointerCasts()) != 0;
  }

  bool identifyUnknownValues() {
    bool any_changed = false;
    bool changed = true;
    while (changed) {
      changed = false;
      for (llvm::Function &function : *module_) {
        for (llvm::Instruction &instruction : llvm::instructions(function)) {
          if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
            if (store->getValueOperand()->getType()->isPointerTy() &&
                isUnknownValue(store->getValueOperand())) {
              const bool inserted =
                  unknown_memory_
                      .insert(store->getPointerOperand()->stripPointerCasts())
                      .second;
              changed = inserted || changed;
              any_changed = inserted || any_changed;
            }
          }
          if (auto *set = llvm::dyn_cast<llvm::MemSetInst>(&instruction)) {
            const bool inserted =
                unknown_memory_.insert(set->getDest()->stripPointerCasts())
                    .second;
            changed = inserted || changed;
            any_changed = inserted || any_changed;
          }
          if (auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
            if (rmw->getValOperand()->getType()->isPointerTy()) {
              const bool inserted =
                  unknown_memory_
                      .insert(rmw->getPointerOperand()->stripPointerCasts())
                      .second;
              changed = inserted || changed;
              any_changed = inserted || any_changed;
            }
          }
          if (auto *exchange =
                  llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction)) {
            if (exchange->getNewValOperand()->getType()->isPointerTy()) {
              const bool inserted =
                  unknown_memory_
                      .insert(
                          exchange->getPointerOperand()->stripPointerCasts())
                      .second;
              changed = inserted || changed;
              any_changed = inserted || any_changed;
            }
          }
          if (auto *transfer =
                  llvm::dyn_cast<llvm::MemTransferInst>(&instruction)) {
            if (unknown_memory_.count(
                    transfer->getSource()->stripPointerCasts()) != 0) {
              const bool inserted =
                  unknown_memory_
                      .insert(transfer->getDest()->stripPointerCasts())
                      .second;
              changed = inserted || changed;
              any_changed = inserted || any_changed;
            }
          }

          if (!instruction.getType()->isPointerTy()) {
            continue;
          }
          bool is_unknown =
              llvm::isa<llvm::ExtractValueInst, llvm::IntToPtrInst,
                        llvm::AtomicRMWInst, llvm::VAArgInst,
                        llvm::ExtractElementInst>(&instruction);
          if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
            is_unknown =
                is_unknown ||
                unknown_memory_.count(
                    load->getPointerOperand()->stripPointerCasts()) != 0;
          }
          for (const llvm::Use &operand : instruction.operands()) {
            if (operand->getType()->isPointerTy() &&
                isUnknownValue(operand.get())) {
              is_unknown = true;
              break;
            }
          }
          if (is_unknown) {
            const bool inserted = unknown_values_.insert(&instruction).second;
            changed = inserted || changed;
            any_changed = inserted || any_changed;
          }
        }
      }
    }
    return any_changed;
  }

  bool refineUnknownValues() {
    bool changed = false;
    for (llvm::Function &function : *module_) {
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
        if (!load || !load->getType()->isPointerTy() || isUnknownValue(load)) {
          continue;
        }
        for (const llvm::Value *unknown_memory : unknown_memory_) {
          const auto source = mappedValue(load->getPointerOperand());
          const auto target = mappedValue(unknown_memory);
          if (!source || !target || client_->mayValueAlias(*source, *target)) {
            changed = unknown_values_.insert(load).second || changed;
            break;
          }
        }
      }
    }
    return identifyUnknownValues() || changed;
  }

  bool addSupplementalConstraints() {
    bool changed = false;
    for (llvm::GlobalVariable &global : module_->globals()) {
      if (supplemented_values_.count(&global) != 0) {
        continue;
      }
      if (!global.hasInitializer() ||
          !global.getInitializer()->getType()->isPointerTy() ||
          global.getInitializer()->isNullValue()) {
        continue;
      }
      const auto source = mappedValue(global.getInitializer());
      const auto target = mappedValue(&global);
      if (source && target) {
        changed = client_->addConstraint(*source, *target,
                                         AliasConstraintEdgeKind::Store) ||
                  changed;
        supplemented_values_.insert(&global);
      }
    }

    for (llvm::Function &function : *module_) {
      for (llvm::Instruction &instruction : llvm::instructions(function)) {
        const auto *copy = llvm::dyn_cast<llvm::MemTransferInst>(&instruction);
        if (!copy) {
          continue;
        }
        if (supplemented_values_.count(&instruction) != 0) {
          continue;
        }
        const auto source = mappedValue(copy->getSource());
        const auto target = mappedValue(copy->getDest());
        if (!source || !target) {
          continue;
        }
        std::optional<std::uint32_t> size;
        if (const auto *constant =
                llvm::dyn_cast<llvm::ConstantInt>(copy->getLength())) {
          if (!constant->isNegative() &&
              constant->getValue().getActiveBits() <= 32) {
            size = static_cast<std::uint32_t>(constant->getZExtValue());
          }
        }
        changed = client_->addMemoryTransfer(*source, *target, size) || changed;
        supplemented_values_.insert(&instruction);
      }
    }
    return changed;
  }

  LLVMAliasOptions options_;
  llvm::Module *module_ = nullptr;
  bool analyzed_ = false;
  std::unique_ptr<ConstraintBuilder> builder_;
  std::unique_ptr<AliasClient> client_;
  std::unique_ptr<Synchronizer> synchronizer_;
  std::unordered_map<const llvm::Value *, std::size_t> value_nodes_;
  std::set<const llvm::Value *> unknown_values_;
  std::set<const llvm::Value *> unknown_memory_;
  std::set<const llvm::Value *> supplemented_values_;
  ReachabilityStats statistics_;
  std::uint64_t frontend_time_microseconds_ = 0;
  std::uint64_t client_initialization_microseconds_ = 0;
  std::uint64_t client_discovery_microseconds_ = 0;
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
