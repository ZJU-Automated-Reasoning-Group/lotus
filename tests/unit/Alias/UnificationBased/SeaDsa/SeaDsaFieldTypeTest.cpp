#include "Alias/UnificationBased/seadsa/FieldType.hh"
#include "Alias/UnificationBased/seadsa/Graph.hh"

#include <gtest/gtest.h>

using namespace llvm;

namespace {

class TypeAwareModeGuard {
public:
  TypeAwareModeGuard() : old_value_(seadsa::g_IsTypeAware) {}
  ~TypeAwareModeGuard() { seadsa::g_IsTypeAware = old_value_; }

private:
  bool old_value_;
};

TEST(SeaDsaFieldTypeTest, TracksGlobalTypeAwareFlagConsistently) {
  LLVMContext context;
  Type *intPtrTy = Type::getInt32PtrTy(context);

  TypeAwareModeGuard restore_mode;

  seadsa::g_IsTypeAware = false;
  EXPECT_TRUE(seadsa::FieldType::IsNotTypeAware());
  EXPECT_TRUE(seadsa::FieldType(intPtrTy).isUnknown());

  seadsa::g_IsTypeAware = true;
  EXPECT_FALSE(seadsa::FieldType::IsNotTypeAware());
  EXPECT_TRUE(seadsa::FieldType(intPtrTy).isPointer());
}

} // namespace
