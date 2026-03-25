#include "Utils/LLVM/StringUtils.h"

#include <string>

#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

TEST(StringUtilsTest, RemovePrefix) {
  std::string base = "prefix_value";
  StringRef ref(base);
  EXPECT_TRUE(remove_prefix(ref, "prefix_"));
  EXPECT_EQ(ref, "value");

  std::string other = "value";
  StringRef ref2(other);
  EXPECT_FALSE(remove_prefix(ref2, "prefix_"));
  EXPECT_EQ(ref2, "value");
}

TEST(StringUtilsTest, DrawSeparateLine) {
  std::string out;
  llvm::raw_string_ostream os(out);
  draw_separate_line(os, 5, '*', false);
  os.flush();
  EXPECT_EQ(out, "*****");

  std::string out_with_newline;
  llvm::raw_string_ostream os2(out_with_newline);
  draw_separate_line(os2, 3, '-', true);
  os2.flush();
  EXPECT_EQ(out_with_newline, "---\n");
}

TEST(StringUtilsTest, OutputPaddedText) {
  std::string out;
  llvm::raw_string_ostream os(out);
  output_padded_text(os, "hi", 10, '-', false);
  os.flush();
  EXPECT_EQ(out, "----hi----");

  std::string no_padding;
  llvm::raw_string_ostream os2(no_padding);
  output_padded_text(os2, "abcdef", 3, '.', false);
  os2.flush();
  EXPECT_EQ(no_padding, "abcdef\n");
}

TEST(StringUtilsTest, OutputAlignedText) {
  std::string left;
  llvm::raw_string_ostream os(left);
  output_left_aligned_text(os, "hi", 6, '.', false);
  os.flush();
  EXPECT_EQ(left, "hi....");

  std::string right;
  llvm::raw_string_ostream os2(right);
  output_right_aligned_text(os2, "hi", 6, '.', false);
  os2.flush();
  EXPECT_EQ(right, "....hi");
}

TEST(StringUtilsTest, OrdinalHelpers) {
  EXPECT_STREQ(ordinal_suffix(1), "st");
  EXPECT_STREQ(ordinal_suffix(2), "nd");
  EXPECT_STREQ(ordinal_suffix(3), "rd");
  EXPECT_STREQ(ordinal_suffix(4), "th");
  EXPECT_STREQ(ordinal_suffix(11), "th");
  EXPECT_STREQ(ordinal_suffix(12), "th");
  EXPECT_STREQ(ordinal_suffix(13), "th");
  EXPECT_STREQ(ordinal_suffix(21), "st");
  EXPECT_STREQ(ordinal_suffix(-2), "nd");
  EXPECT_EQ(ordinal_string(42), "42nd");
}

TEST(StringUtilsTest, BinaryString) {
  EXPECT_EQ(to_binary_string(10), "1010");
  EXPECT_EQ(to_binary_string(1), "1");
  EXPECT_EQ(to_binary_string(0), "");
}

TEST(StringUtilsTest, HtmlEscapeString) {
  std::string input = "A & \"B\" <C> 'D'";
  std::string expected =
      "A&nbsp;&amp;&nbsp;&quot;B&quot;&nbsp;&lt;C&gt;&nbsp;&#39;D&#39;";
  EXPECT_EQ(html_escape_string(input), expected);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
