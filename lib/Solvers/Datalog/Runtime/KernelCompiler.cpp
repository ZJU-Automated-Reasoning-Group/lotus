#include "Solvers/Datalog/EngineInternal.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

namespace lotus::datalog::internal {
namespace {

struct PrimitiveType {
  unsigned bits = 0;
  bool is_signed = false;
};

std::optional<PrimitiveType> primitiveType(std::type_index type) {
  if (type == typeid(bool))
    return PrimitiveType{1, false};
  if (type == typeid(int))
    return PrimitiveType{static_cast<unsigned>(sizeof(int) * 8), true};
  if (type == typeid(std::int64_t))
    return PrimitiveType{64, true};
  if (type == typeid(std::uint64_t))
    return PrimitiveType{64, false};
  if (type == typeid(std::size_t))
    return PrimitiveType{static_cast<unsigned>(sizeof(std::size_t) * 8), false};
  return std::nullopt;
}

std::optional<std::uint64_t> constantBits(const ExprNode &node) {
  if (node.type == typeid(bool))
    return std::any_cast<bool>(node.constant) ? 1 : 0;
  if (node.type == typeid(int))
    return static_cast<std::uint64_t>(std::any_cast<int>(node.constant));
  if (node.type == typeid(std::int64_t))
    return static_cast<std::uint64_t>(
        std::any_cast<std::int64_t>(node.constant));
  if (node.type == typeid(std::uint64_t))
    return std::any_cast<std::uint64_t>(node.constant);
  if (node.type == typeid(std::size_t))
    return static_cast<std::uint64_t>(
        std::any_cast<std::size_t>(node.constant));
  return std::nullopt;
}

class KernelState {
public:
  KernelState() {
    static std::once_flag target_once;
    std::call_once(target_once, [] {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
    });
    auto created = llvm::orc::LLJITBuilder().create();
    if (!created) {
      llvm::consumeError(created.takeError());
      return;
    }
    jit_ = std::move(*created);
  }

  bool available() const { return static_cast<bool>(jit_); }

  using Function = std::uint64_t (*)(const std::uint64_t *);

  std::optional<Function> compile(const std::shared_ptr<ExprNode> &node,
                                  std::string &name) {
    if (!jit_ || !node || !supported(*node))
      return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);
    name = "lotus_datalog_expr_" + std::to_string(next_id_++);
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(name, *context);
    llvm::IRBuilder<> builder(*context);
    llvm::Type *i64 = builder.getInt64Ty();
    auto *function_type = llvm::FunctionType::get(
        i64, {llvm::PointerType::getUnqual(i64)}, false);
    llvm::Function *function = llvm::Function::Create(
        function_type, llvm::Function::ExternalLinkage, name, *module);
    llvm::BasicBlock *entry =
        llvm::BasicBlock::Create(*context, "entry", function);
    builder.SetInsertPoint(entry);
    llvm::Value *bindings = function->arg_begin();
    llvm::Value *value = emit(*node, bindings, builder);
    if (!value)
      return std::nullopt;
    const PrimitiveType result_type = *primitiveType(node->type);
    if (value->getType()->getIntegerBitWidth() < 64) {
      value = result_type.is_signed ? builder.CreateSExt(value, i64)
                                    : builder.CreateZExt(value, i64);
    }
    builder.CreateRet(value);

    if (llvm::Error error = jit_->addIRModule(llvm::orc::ThreadSafeModule(
            std::move(module), std::move(context)))) {
      llvm::consumeError(std::move(error));
      return std::nullopt;
    }
    auto symbol = jit_->lookup(name);
    if (!symbol) {
      llvm::consumeError(symbol.takeError());
      return std::nullopt;
    }
    return reinterpret_cast<Function>(
        static_cast<std::uintptr_t>(symbol->getAddress()));
  }

private:
  bool supported(const ExprNode &node) const {
    if (!primitiveType(node.type))
      return false;
    if (node.opcode == ExprOpcode::Constant)
      return constantBits(node).has_value();
    if (node.opcode == ExprOpcode::Variable)
      return true;
    if (!node.lhs || !supported(*node.lhs))
      return false;
    if (node.opcode != ExprOpcode::Negate &&
        node.opcode != ExprOpcode::Positive &&
        node.opcode != ExprOpcode::LogicalNot &&
        (!node.rhs || !supported(*node.rhs)))
      return false;
    return true;
  }

  llvm::Value *emit(const ExprNode &node, llvm::Value *bindings,
                    llvm::IRBuilder<> &builder) {
    const PrimitiveType type = *primitiveType(node.type);
    llvm::IntegerType *integer_type = builder.getIntNTy(type.bits);
    if (node.opcode == ExprOpcode::Constant)
      return llvm::ConstantInt::get(integer_type, *constantBits(node));
    if (node.opcode == ExprOpcode::Variable) {
      llvm::Value *address = builder.CreateInBoundsGEP(
          builder.getInt64Ty(), bindings, builder.getInt64(node.variable));
      llvm::Value *loaded = builder.CreateLoad(builder.getInt64Ty(), address);
      return type.bits == 64 ? loaded
                             : builder.CreateTrunc(loaded, integer_type);
    }

    llvm::Value *lhs = emit(*node.lhs, bindings, builder);
    if (!lhs)
      return nullptr;
    if (node.opcode == ExprOpcode::Negate)
      return builder.CreateNeg(lhs);
    if (node.opcode == ExprOpcode::Positive)
      return lhs;
    if (node.opcode == ExprOpcode::LogicalNot)
      return builder.CreateNot(lhs);
    llvm::Value *rhs = emit(*node.rhs, bindings, builder);
    if (!rhs)
      return nullptr;
    const PrimitiveType operand_type = *primitiveType(node.lhs->type);
    switch (node.opcode) {
    case ExprOpcode::Add:
      return builder.CreateAdd(lhs, rhs);
    case ExprOpcode::Subtract:
      return builder.CreateSub(lhs, rhs);
    case ExprOpcode::Multiply:
      return builder.CreateMul(lhs, rhs);
    case ExprOpcode::Divide:
      return operand_type.is_signed ? builder.CreateSDiv(lhs, rhs)
                                    : builder.CreateUDiv(lhs, rhs);
    case ExprOpcode::Remainder:
      return operand_type.is_signed ? builder.CreateSRem(lhs, rhs)
                                    : builder.CreateURem(lhs, rhs);
    case ExprOpcode::Equal:
      return builder.CreateICmpEQ(lhs, rhs);
    case ExprOpcode::NotEqual:
      return builder.CreateICmpNE(lhs, rhs);
    case ExprOpcode::Less:
      return operand_type.is_signed ? builder.CreateICmpSLT(lhs, rhs)
                                    : builder.CreateICmpULT(lhs, rhs);
    case ExprOpcode::LessEqual:
      return operand_type.is_signed ? builder.CreateICmpSLE(lhs, rhs)
                                    : builder.CreateICmpULE(lhs, rhs);
    case ExprOpcode::Greater:
      return operand_type.is_signed ? builder.CreateICmpSGT(lhs, rhs)
                                    : builder.CreateICmpUGT(lhs, rhs);
    case ExprOpcode::GreaterEqual:
      return operand_type.is_signed ? builder.CreateICmpSGE(lhs, rhs)
                                    : builder.CreateICmpUGE(lhs, rhs);
    case ExprOpcode::LogicalAnd:
      return builder.CreateAnd(lhs, rhs);
    case ExprOpcode::LogicalOr:
      return builder.CreateOr(lhs, rhs);
    default:
      return nullptr;
    }
  }

  std::unique_ptr<llvm::orc::LLJIT> jit_;
  std::mutex mutex_;
  std::uint64_t next_id_ = 0;
};

KernelState &kernelState() {
  static KernelState state;
  return state;
}

void collectVariables(const std::shared_ptr<ExprNode> &node,
                      std::unordered_map<VarId, std::type_index> &variables) {
  if (!node)
    return;
  if (node->opcode == ExprOpcode::Variable)
    variables.emplace(node->variable, node->type);
  collectVariables(node->lhs, variables);
  collectVariables(node->rhs, variables);
}

bool compileExpression(ExprIR &expression) {
  if (!expression.jit_safe)
    return false;
  std::string name;
  auto compiled = kernelState().compile(expression.node, name);
  if (!compiled)
    return false;
  std::unordered_map<VarId, std::type_index> variable_map;
  collectVariables(expression.node, variable_map);
  std::vector<std::pair<VarId, std::type_index>> variables(variable_map.begin(),
                                                           variable_map.end());
  const std::type_index result_type = expression.type;
  const KernelState::Function function = *compiled;
  expression.evaluate = [function, variables = std::move(variables),
                         result_type](const Binding &binding) -> std::any {
    std::vector<std::uint64_t> packed(binding.size(), 0);
    for (const auto &[variable, type] : variables) {
      if (variable >= binding.size() || !binding[variable])
        throw std::logic_error("evaluating an unbound JIT variable");
      if (type == typeid(bool))
        packed[variable] = binding[variable].get<bool>() ? 1 : 0;
      else if (type == typeid(int))
        packed[variable] =
            static_cast<std::uint64_t>(binding[variable].get<int>());
      else if (type == typeid(std::int64_t))
        packed[variable] =
            static_cast<std::uint64_t>(binding[variable].get<std::int64_t>());
      else if (type == typeid(std::uint64_t))
        packed[variable] = binding[variable].get<std::uint64_t>();
      else if (type == typeid(std::size_t))
        packed[variable] =
            static_cast<std::uint64_t>(binding[variable].get<std::size_t>());
    }
    const std::uint64_t result = function(packed.data());
    if (result_type == typeid(bool))
      return std::any(result != 0);
    if (result_type == typeid(int))
      return std::any(static_cast<int>(result));
    if (result_type == typeid(std::int64_t))
      return std::any(static_cast<std::int64_t>(result));
    if (result_type == typeid(std::uint64_t))
      return std::any(result);
    if (result_type == typeid(std::size_t))
      return std::any(static_cast<std::size_t>(result));
    throw std::logic_error("unsupported JIT expression result type");
  };
  expression.compiled_kernel = true;
  return true;
}

} // namespace

std::size_t compileRuleKernels(std::vector<RulePlan> &rules) {
  std::size_t compiled = 0;
  if (!kernelState().available())
    return compiled;
  for (RulePlan &rule : rules) {
    for (HeadTermPlan &term : rule.head) {
      if (term.kind == HeadTermPlan::Kind::Expression)
        compiled += compileExpression(term.expression);
    }
    for (PhysicalOp &operation : rule.body) {
      if (operation.code == OpCode::Filter)
        compiled += compileExpression(operation.filter);
      if (operation.code == OpCode::Aggregate) {
        compiled += compileExpression(operation.aggregate.projection);
        for (AggregateSourceOp &source : operation.aggregate_body) {
          if (source.code == OpCode::Filter)
            compiled += compileExpression(source.filter);
        }
      }
    }
  }
  return compiled;
}

} // namespace lotus::datalog::internal
