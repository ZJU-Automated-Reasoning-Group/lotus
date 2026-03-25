#include "Utils/Parallel/ThreadSafe.h"

#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace {

using lotus::ThreadSafeMap;
using lotus::ThreadSafeSet;
using lotus::ThreadSafeVector;

struct ThrowingOptionalValue {
  static bool ThrowOnCopy;
  static int DoubleDeleteCount;
  static std::set<void *> DeletedPointers;

  int value = 0;

  ThrowingOptionalValue() = default;
  explicit ThrowingOptionalValue(int V) : value(V) {}

  ThrowingOptionalValue(const ThrowingOptionalValue &Other) : value(Other.value) {
    if (ThrowOnCopy)
      throw std::runtime_error("copy boom");
  }

  static void resetTracking() {
    ThrowOnCopy = false;
    DoubleDeleteCount = 0;
    DeletedPointers.clear();
  }

  static void *operator new(std::size_t Size) { return ::operator new(Size); }

  static void operator delete(void *Ptr) noexcept {
    if (Ptr == nullptr)
      return;
    if (!DeletedPointers.insert(Ptr).second) {
      ++DoubleDeleteCount;
      return;
    }
  }

  static void operator delete(void *Ptr, std::size_t) noexcept {
    operator delete(Ptr);
  }
};

bool ThrowingOptionalValue::ThrowOnCopy = false;
int ThrowingOptionalValue::DoubleDeleteCount = 0;
std::set<void *> ThrowingOptionalValue::DeletedPointers;

TEST(ThreadSafeTest, SetSnapshotAndEraseReflectContents) {
  ThreadSafeSet<int> values;
  EXPECT_TRUE(values.insert(1));
  EXPECT_TRUE(values.insert(2));
  EXPECT_FALSE(values.insert(2));

  auto snapshot = values.snapshot();
  std::set<int> ordered(snapshot.begin(), snapshot.end());
  EXPECT_EQ(ordered, (std::set<int>{1, 2}));

  EXPECT_TRUE(values.erase(1));
  EXPECT_FALSE(values.contains(1));
  EXPECT_FALSE(values.erase(3));
}

TEST(ThreadSafeTest, MapSnapshotEraseAndUpdateWorkTogether) {
  ThreadSafeMap<std::string, int> values;
  EXPECT_TRUE(values.insert_or_assign("a", 1));
  EXPECT_FALSE(values.insert_or_assign("a", 2));
  EXPECT_TRUE(values.insert_or_assign("b", 3));

  EXPECT_TRUE(values.update("a", [](int &value) { value += 5; }));
  EXPECT_FALSE(values.update("missing", [](int &) {}));

  auto snapshot = values.snapshot();
  EXPECT_EQ(snapshot.at("a"), 7);
  EXPECT_EQ(snapshot.at("b"), 3);

  EXPECT_TRUE(values.erase("b"));
  EXPECT_FALSE(values.contains("b"));
  EXPECT_FALSE(values.erase("b"));
}

TEST(ThreadSafeTest, VectorSnapshotPreservesInsertionOrder) {
  ThreadSafeVector<int> values;
  values.push_back(4);
  values.push_back(5);
  values.push_back(6);

  EXPECT_EQ(values.snapshot(), (std::vector<int>{4, 5, 6}));
}

TEST(ThreadSafeTest, SimpleOptionalAssignmentPreservesStateOnCopyFailure) {
  ThrowingOptionalValue::resetTracking();

  {
    lotus::SimpleOptional<ThrowingOptionalValue> source(
        ThrowingOptionalValue(7));
    lotus::SimpleOptional<ThrowingOptionalValue> dest(ThrowingOptionalValue(3));

    ThrowingOptionalValue::ThrowOnCopy = true;
    EXPECT_THROW(dest = source, std::runtime_error);

    EXPECT_TRUE(dest.has_value());
    EXPECT_EQ(dest.value().value, 3);
  }

  EXPECT_EQ(ThrowingOptionalValue::DoubleDeleteCount, 0);
}

} // namespace
