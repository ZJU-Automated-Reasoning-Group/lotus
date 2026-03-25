#ifndef PCOMB_INPUT_STREAM_H
#define PCOMB_INPUT_STREAM_H

#include <cassert>

#include <llvm/ADT/StringRef.h>

namespace pcomb {

class InputStream {
private:
  struct Position {
    size_t row, col;
  };

  llvm::StringRef str;
  Position pos;

  InputStream(const llvm::StringRef &s, const Position &p) : str(s), pos(p) {}

public:
  InputStream(llvm::StringRef s) : str(s), pos({1, 1}) {}

  bool isEOF() const { return str.empty(); }

  const char *getRawBuffer() const { return str.data(); }

  InputStream consume(size_t n) const {
    assert(n <= str.size());
    auto newPos = pos;
    for (auto i = 0u; i < n; ++i) {
      auto ch = str[i];
      if (ch == '\n') {
        ++newPos.row;
        newPos.col = 1;
      } else
        ++newPos.col;
    }
    return InputStream(str.substr(n), newPos);
  }

  size_t getLineNumber() const { return pos.row; }
  size_t getColumnNumber() const { return pos.col; }
};

} // namespace pcomb

#endif
