#include "IR/PDG/Core/PDGNode.h"
#include "IR/PDG/Support/DebugInfoUtils.h"
#include "IR/PDG/Support/PDGUtils.h"

#include "TestUtils/LLVMHelpers.h"

#include <climits>
#include <memory>
#include <vector>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

constexpr const char *kDataLayout =
    "e-m:e-p:64:64-i64:64-n8:16:32:64-S128";

class PDGSupportUtilsTest : public ::testing::Test {
protected:
  std::unique_ptr<Module> parseModule(const char *source) {
    auto module =
        lotus::unittest::parseModule(context_, source, "PDGSupportUtilsTest");
    if (module)
      module->setDataLayout(kDataLayout);
    return module;
  }

  template <typename InstTy>
  std::vector<InstTy *> collectInstructions(Function &function) {
    std::vector<InstTy *> instructions;
    for (BasicBlock &block : function)
      for (Instruction &inst : block)
        if (auto *candidate = dyn_cast<InstTy>(&inst))
          instructions.push_back(candidate);
    return instructions;
  }

  LLVMContext context_;
};

TEST_F(PDGSupportUtilsTest, GEPHelpersComputeOffsetsAndMatchDebugOffsets) {
  auto module = parseModule(R"(
    %struct.S = type { i32, i8 }

    define void @test(%struct.S* %p, i32* %q, i32 %idx) {
    entry:
      %field = getelementptr inbounds %struct.S, %struct.S* %p, i32 0, i32 1
      %dynamic = getelementptr inbounds i32, i32* %q, i32 %idx
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("test");
  ASSERT_NE(function, nullptr);
  auto *field_gep = findInstruction<GetElementPtrInst>(*function, "field");
  auto *dynamic_gep = findInstruction<GetElementPtrInst>(*function, "dynamic");
  ASSERT_NE(field_gep, nullptr);
  ASSERT_NE(dynamic_gep, nullptr);

  auto *struct_type = pdg::pdgutils::getStructTypeFromGEP(*field_gep);
  ASSERT_NE(struct_type, nullptr);
  EXPECT_EQ(pdg::pdgutils::getGEPAccessFieldOffset(*field_gep), 1);
  EXPECT_EQ(pdg::pdgutils::getGEPAccessFieldOffset(*dynamic_gep), INT_MIN);
  EXPECT_EQ(pdg::pdgutils::getGEPOffsetInBits(*module, *struct_type, *field_gep),
            32);

  DIBuilder builder(*module);
  DIFile *file = builder.createFile("gep.c", "/tmp");
  builder.createCompileUnit(dwarf::DW_LANG_C99, file, "PDGSupportUtilsTest",
                            false, "", 0);
  DIType *int_type = builder.createBasicType("int", 32, dwarf::DW_ATE_signed);
  DIType *char_type =
      builder.createBasicType("char", 8, dwarf::DW_ATE_signed_char);
  auto *field0 = builder.createMemberType(file, "a", file, 1, 32, 32, 0,
                                          DINode::FlagZero, int_type);
  auto *field1 = builder.createMemberType(file, "b", file, 1, 8, 8, 32,
                                          DINode::FlagZero, char_type);
  builder.createStructType(file, "S", file, 1, 64, 32, DINode::FlagZero,
                           nullptr, builder.getOrCreateArray({field0, field1}));
  builder.finalize();

  ASSERT_NE(field1, nullptr);
  EXPECT_TRUE(pdg::pdgutils::isGEPOffsetMatchDIOffset(*field1, *field_gep));

  pdg::Node node(pdg::GraphNodeType::INST_OTHER);
  node.setDIType(*field1);
  EXPECT_TRUE(pdg::pdgutils::isNodeBitOffsetMatchGEPBitOffset(node, *field_gep));
}

TEST_F(PDGSupportUtilsTest, GetCalledFuncHandlesDirectBitcastAndIndirectCalls) {
  auto module = parseModule(R"(
    define void @callee(i32 %x) {
    entry:
      ret void
    }

    define void @caller(void (i32)* %fp, i32 %x) {
    entry:
      call void @callee(i32 %x)
      call void bitcast (void (i32)* @callee to void (i32)*)(i32 %x)
      call void %fp(i32 %x)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  auto calls = collectInstructions<CallInst>(*caller);
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(pdg::pdgutils::getCalledFunc(*calls[0]), callee);
  EXPECT_EQ(pdg::pdgutils::getCalledFunc(*calls[1]), callee);
  EXPECT_EQ(pdg::pdgutils::getCalledFunc(*calls[2]), nullptr);
}

TEST_F(PDGSupportUtilsTest, InstructionTraversalHelpersReturnExpectedSets) {
  auto module = parseModule(R"(
    define i32 @f(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = mul i32 %a, 2
      %c = sub i32 %b, 3
      ret i32 %c
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  auto *inst_a = findInstruction<Instruction>(*function, "a");
  auto *inst_b = findInstruction<Instruction>(*function, "b");
  auto *inst_c = findInstruction<Instruction>(*function, "c");
  ASSERT_NE(inst_a, nullptr);
  ASSERT_NE(inst_b, nullptr);
  ASSERT_NE(inst_c, nullptr);

  std::set<Instruction *> before_b = pdg::pdgutils::getInstructionBeforeInst(*inst_b);
  std::set<Instruction *> after_b = pdg::pdgutils::getInstructionAfterInst(*inst_b);
  EXPECT_TRUE(before_b.count(inst_a));
  EXPECT_FALSE(before_b.count(inst_b));
  EXPECT_FALSE(before_b.count(inst_c));
  EXPECT_TRUE(after_b.count(inst_c));
  EXPECT_FALSE(after_b.count(inst_a));
}

TEST_F(PDGSupportUtilsTest, ComputeAddrTakenVarsFromAllocTracksForwardingAndEscapes) {
  auto module = parseModule(R"(
    declare i8** @sink(i8**)

    define i8** @test(i1 %cond, i8*** %out) {
    entry:
      %slot = alloca i8*, align 8
      %cast = bitcast i8** %slot to i8**
      %gep = getelementptr i8*, i8** %slot, i64 0
      %ld = load i8*, i8** %slot, align 8
      br i1 %cond, label %then, label %else

    then:
      br label %merge

    else:
      br label %merge

    merge:
      %phi = phi i8** [ %cast, %then ], [ %gep, %else ]
      %sel = select i1 %cond, i8** %phi, i8** %slot
      store i8** %sel, i8*** %out, align 8
      %call = call i8** @sink(i8** %sel)
      ret i8** %sel
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *function = module->getFunction("test");
  ASSERT_NE(function, nullptr);
  auto *slot = findInstruction<AllocaInst>(*function, "slot");
  auto *cast_inst = findInstruction<BitCastInst>(*function, "cast");
  auto *gep = findInstruction<GetElementPtrInst>(*function, "gep");
  auto *load = findInstruction<LoadInst>(*function, "ld");
  auto *phi = findInstruction<PHINode>(*function, "phi");
  auto *sel = findInstruction<SelectInst>(*function, "sel");
  auto *call = findInstruction<CallInst>(*function, "call");
  auto *ret = findInstruction<ReturnInst>(*function);
  ASSERT_NE(slot, nullptr);
  ASSERT_NE(cast_inst, nullptr);
  ASSERT_NE(gep, nullptr);
  ASSERT_NE(load, nullptr);
  ASSERT_NE(phi, nullptr);
  ASSERT_NE(sel, nullptr);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(ret, nullptr);

  Value *out_arg = function->getArg(1);
  ASSERT_NE(out_arg, nullptr);

  std::set<Value *> addr_taken =
      pdg::pdgutils::computeAddrTakenVarsFromAlloc(*slot);
  EXPECT_TRUE(addr_taken.count(cast_inst));
  EXPECT_TRUE(addr_taken.count(gep));
  EXPECT_TRUE(addr_taken.count(load));
  EXPECT_TRUE(addr_taken.count(phi));
  EXPECT_TRUE(addr_taken.count(sel));
  EXPECT_TRUE(addr_taken.count(out_arg));
  EXPECT_TRUE(addr_taken.count(call));
  EXPECT_TRUE(addr_taken.count(ret));
}

TEST_F(PDGSupportUtilsTest, DebugInfoTypeHelpersHandleQualifiedAndMissingTypes) {
  auto module = std::make_unique<Module>("dbg-types", context_);
  DIBuilder builder(*module);
  DIFile *file = builder.createFile("types.c", "/tmp");
  builder.createCompileUnit(dwarf::DW_LANG_C99, file, "PDGSupportUtilsTest",
                            false, "", 0);

  DIType *int_type = builder.createBasicType("int", 32, dwarf::DW_ATE_signed);
  auto *const_type =
      builder.createQualifiedType(dwarf::DW_TAG_const_type, int_type);
  auto *volatile_type =
      builder.createQualifiedType(dwarf::DW_TAG_volatile_type, const_type);
  auto *typedef_type = builder.createTypedef(volatile_type, "alias_t", file, 1,
                                             file);
  auto *pointer_type = builder.createPointerType(typedef_type, 64);
  auto *void_pointer_type = builder.createPointerType(nullptr, 64);
  auto *member_type = builder.createMemberType(file, "field", file, 1, 32, 32,
                                               0, DINode::FlagZero, int_type);
  auto *anonymous_struct = builder.createStructType(
      file, "", file, 1, 32, 32, DINode::FlagZero, nullptr,
      builder.getOrCreateArray({member_type}));
  auto *broken_member =
      builder.createMemberType(file, "broken", file, 1, 32, 32, 0,
                               DINode::FlagZero, nullptr);
  auto *broken_const =
      builder.createQualifiedType(dwarf::DW_TAG_const_type, nullptr);
  auto *subroutine_type =
      builder.createSubroutineType(builder.getOrCreateTypeArray({int_type}));
  auto *subprogram = builder.createFunction(
      file, "f", "f", file, 1, subroutine_type, 1, DINode::FlagZero,
      DISubprogram::SPFlagDefinition);
  auto *local_var = builder.createAutoVariable(subprogram, "local", file, 1,
                                               int_type);
  builder.finalize();

  ASSERT_NE(typedef_type, nullptr);
  ASSERT_NE(pointer_type, nullptr);
  ASSERT_NE(void_pointer_type, nullptr);
  ASSERT_NE(member_type, nullptr);
  ASSERT_NE(anonymous_struct, nullptr);
  ASSERT_NE(local_var, nullptr);

  EXPECT_EQ(pdg::dbgutils::stripAttributes(*typedef_type), int_type);
  EXPECT_EQ(pdg::dbgutils::getLowestDIType(*pointer_type), int_type);
  EXPECT_EQ(pdg::dbgutils::stripMemberTag(*member_type), int_type);
  ASSERT_NE(broken_member, nullptr);
  EXPECT_EQ(pdg::dbgutils::stripMemberTag(*broken_member), nullptr);
  ASSERT_NE(broken_const, nullptr);
  EXPECT_EQ(pdg::dbgutils::stripAttributes(*broken_const), nullptr);

  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*const_type), "const int");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*volatile_type),
            "volatile const int");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*void_pointer_type), "void*");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*anonymous_struct), "struct");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*broken_member), "");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelTypeName(*broken_const), "const");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelVariableName(*member_type), "field");
  EXPECT_EQ(pdg::dbgutils::getSourceLevelVariableName(*local_var), "local");
}

TEST_F(PDGSupportUtilsTest, DebugInfoQueriesHandlePresentAndMissingMetadata) {
  auto module = std::make_unique<Module>("dbg-queries", context_);
  DIBuilder builder(*module);
  DIFile *file = builder.createFile("queries.c", "/tmp");
  builder.createCompileUnit(dwarf::DW_LANG_C99, file, "PDGSupportUtilsTest",
                            false, "", 0);

  Type *i32 = Type::getInt32Ty(context_);
  DIType *int_type = builder.createBasicType("int", 32, dwarf::DW_ATE_signed);
  auto *subroutine_type =
      builder.createSubroutineType(builder.getOrCreateTypeArray({int_type, int_type}));

  auto *with_debug = Function::Create(
      FunctionType::get(i32, {i32}, false), GlobalValue::ExternalLinkage,
      "with_debug", *module);
  auto *without_debug = Function::Create(
      FunctionType::get(i32, {i32}, false), GlobalValue::ExternalLinkage,
      "without_debug", *module);

  auto *subprogram = builder.createFunction(
      file, "with_debug", "with_debug", file, 1, subroutine_type, 1,
      DINode::FlagZero, DISubprogram::SPFlagDefinition);
  with_debug->setSubprogram(subprogram);

  auto *global_with_debug = new GlobalVariable(
      *module, i32, false, GlobalValue::ExternalLinkage,
      ConstantInt::get(i32, 0), "global_with_debug");
  auto *global_without_debug = new GlobalVariable(
      *module, i32, false, GlobalValue::ExternalLinkage,
      ConstantInt::get(i32, 0), "global_without_debug");
  global_with_debug->addDebugInfo(builder.createGlobalVariableExpression(
      file, "global_with_debug", "global_with_debug", file, 1, int_type,
      false));

  builder.finalize();

  EXPECT_EQ(pdg::dbgutils::getFuncRetDIType(*with_debug), int_type);
  EXPECT_EQ(pdg::dbgutils::getFuncRetDIType(*without_debug), nullptr);
  EXPECT_EQ(pdg::dbgutils::getGlobalVarDIType(*global_with_debug), int_type);
  EXPECT_EQ(pdg::dbgutils::getGlobalVarDIType(*global_without_debug), nullptr);
}

} // namespace
