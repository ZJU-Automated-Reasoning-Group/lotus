#pragma once

#include <cassert>
#include <cstdint>
#include <new>

namespace annotation {

class ArgPosition {
private:
  uint8_t argIdx;
  bool afterArgs;

public:
  ArgPosition(uint8_t i) : argIdx(i), afterArgs(false) {}
  ArgPosition(uint8_t i, bool a) : argIdx(i), afterArgs(a) {}

  uint8_t getArgIndex() const { return argIdx; }
  bool isAfterArgPosition() const { return afterArgs; }
};

class RetPosition {
public:
  RetPosition() = default;
};

class APosition {
private:
  enum class APositionType : bool {
    Arg,
    Ret,
  };

  APositionType type;
  union {
    ArgPosition argPos;
    RetPosition retPos;
  };

  APosition(uint8_t i, bool r, bool a) {
    if (r) {
      type = APositionType::Ret;
      new (&retPos) RetPosition();
    } else {
      type = APositionType::Arg;
      new (&argPos) ArgPosition(i, a);
    }
  }

public:
  // Copy constructor: manually copy the active union member.
  APosition(const APosition &rhs) : type(rhs.type) {
    switch (type) {
    case APositionType::Arg:
      new (&argPos) ArgPosition(rhs.argPos);
      break;
    case APositionType::Ret:
      new (&retPos) RetPosition();
      break;
    }
  }

  // Copy-assignment operator: destroy the current active member, then
  // copy-construct the new one.  Without this the compiler-generated
  // assignment is deleted (because of the union), causing compile errors
  // whenever APosition objects are assigned rather than copy-constructed.
  APosition &operator=(const APosition &rhs) {
    if (this == &rhs)
      return *this;

    // Destroy current active member.
    switch (type) {
    case APositionType::Arg:
      argPos.~ArgPosition();
      break;
    case APositionType::Ret:
      retPos.~RetPosition();
      break;
    }

    // Copy-construct the new active member.
    type = rhs.type;
    switch (type) {
    case APositionType::Arg:
      new (&argPos) ArgPosition(rhs.argPos);
      break;
    case APositionType::Ret:
      new (&retPos) RetPosition();
      break;
    }
    return *this;
  }

  ~APosition() {
    switch (type) {
    case APositionType::Arg:
      argPos.~ArgPosition();
      break;
    case APositionType::Ret:
      retPos.~RetPosition();
      break;
    }
  }

  bool isReturnPosition() const { return type == APositionType::Ret; }
  bool isArgPosition() const { return type == APositionType::Arg; }

  const ArgPosition &getAsArgPosition() const {
    assert(type == APositionType::Arg);
    return argPos;
  }
  const RetPosition &getAsRetPosition() const {
    assert(type == APositionType::Ret);
    return retPos;
  }

  static APosition getReturnPosition() { return APosition(0, true, false); }
  static APosition getArgPosition(uint8_t idx) {
    return APosition(idx, false, false);
  }
  static APosition getAfterArgPosition(uint8_t idx) {
    return APosition(idx, false, true);
  }
};

} // namespace annotation
