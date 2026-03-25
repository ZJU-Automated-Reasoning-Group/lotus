#pragma once

#include "Annotation/ArgPosition.h"

#include <utility> // std::move

namespace annotation {

enum class PointerEffectType {
  Alloc,
  Copy,
  Exit,
};

class PointerAllocEffect {
private:
  APosition sizePos;
  bool hasSize;

public:
  PointerAllocEffect()
      : sizePos(APosition::getReturnPosition()), hasSize(false) {}
  PointerAllocEffect(const APosition &p) : sizePos(p), hasSize(true) {}

  bool hasSizePosition() const { return hasSize; }
  APosition getSizePosition() const {
    assert(hasSize);
    return sizePos;
  }
};

class CopySource {
public:
  enum class SourceType : std::uint8_t {
    Value,
    DirectMemory,
    ReachableMemory,
    Null,
    Universal,
    Static,
  };

private:
  SourceType type;
  APosition pos;

  CopySource(SourceType t, const APosition &p) : type(t), pos(p) {}

public:
  SourceType getType() const { return type; }
  APosition getPosition() const {
    assert(pos.isArgPosition());
    return pos;
  }

  static CopySource getValue(const APosition &p) {
    assert(p.isArgPosition());
    return CopySource(SourceType::Value, p);
  }
  static CopySource getDirectMemory(const APosition &p) {
    assert(p.isArgPosition());
    return CopySource(SourceType::DirectMemory, p);
  }
  static CopySource getReachableMemory(const APosition &p) {
    assert(p.isArgPosition());
    return CopySource(SourceType::ReachableMemory, p);
  }
  static CopySource getNullPointer() {
    return CopySource(SourceType::Null, APosition::getReturnPosition());
  }
  static CopySource getUniversalPointer() {
    return CopySource(SourceType::Universal, APosition::getReturnPosition());
  }
  static CopySource getStaticPointer() {
    return CopySource(SourceType::Static, APosition::getReturnPosition());
  }
};

class CopyDest {
public:
  enum class DestType : std::uint8_t {
    Value,
    DirectMemory,
    ReachableMemory,
  };

private:
  DestType type;
  APosition pos;

  CopyDest(DestType t, const APosition &p) : type(t), pos(p) {}

public:
  DestType getType() const { return type; }
  APosition getPosition() const { return pos; }

  static CopyDest getValue(const APosition &p) {
    return CopyDest(DestType::Value, p);
  }
  static CopyDest getDirectMemory(const APosition &p) {
    return CopyDest(DestType::DirectMemory, p);
  }
  static CopyDest getReachableMemory(const APosition &p) {
    return CopyDest(DestType::ReachableMemory, p);
  }
};

class PointerCopyEffect {
private:
  CopyDest dstPos;
  CopySource srcPos;

public:
  PointerCopyEffect(const CopyDest &d, const CopySource &s)
      : dstPos(d), srcPos(s) {}

  const CopyDest &getDest() const { return dstPos; }
  const CopySource &getSource() const { return srcPos; }
};

class PointerExitEffect {
public:
  PointerExitEffect() {}
};

class PointerEffect {
private:
  PointerEffectType type;

  union {
    PointerAllocEffect alloc;
    PointerCopyEffect copy;
    PointerExitEffect eexit;
  };

  PointerEffect(const PointerExitEffect &) : type(PointerEffectType::Exit) {
    new (&eexit) PointerExitEffect();
  }
  PointerEffect() : type(PointerEffectType::Alloc) {
    new (&alloc) PointerAllocEffect();
  }
  PointerEffect(const APosition &a) : type(PointerEffectType::Alloc) {
    new (&alloc) PointerAllocEffect(a);
  }
  PointerEffect(const CopyDest &d, const CopySource &s)
      : type(PointerEffectType::Copy) {
    new (&copy) PointerCopyEffect(d, s);
  }

public:
  PointerEffect(const PointerEffect &rhs) : type(rhs.type) {
    switch (type) {
    case PointerEffectType::Alloc:
      new (&alloc) PointerAllocEffect(rhs.alloc);
      break;
    case PointerEffectType::Copy:
      new (&copy) PointerCopyEffect(rhs.copy);
      break;
    case PointerEffectType::Exit:
      new (&eexit) PointerExitEffect();
      break;
    }
  }

  // Copy-assignment operator: required because the union contains members
  // with non-trivial copy assignment, which causes the compiler-generated
  // operator= to be deleted.  Without this, storing PointerEffect in
  // standard containers or assigning between instances fails to compile.
  PointerEffect &operator=(const PointerEffect &rhs) {
    if (this == &rhs)
      return *this;

    // Destroy the currently active union member.
    switch (type) {
    case PointerEffectType::Alloc:
      alloc.~PointerAllocEffect();
      break;
    case PointerEffectType::Copy:
      copy.~PointerCopyEffect();
      break;
    case PointerEffectType::Exit:
      eexit.~PointerExitEffect();
      break;
    }

    // Copy-construct the new active member.
    type = rhs.type;
    switch (type) {
    case PointerEffectType::Alloc:
      new (&alloc) PointerAllocEffect(rhs.alloc);
      break;
    case PointerEffectType::Copy:
      new (&copy) PointerCopyEffect(rhs.copy);
      break;
    case PointerEffectType::Exit:
      new (&eexit) PointerExitEffect();
      break;
    }
    return *this;
  }

  // Move constructor.
  PointerEffect(PointerEffect &&rhs) noexcept : type(rhs.type) {
    switch (type) {
    case PointerEffectType::Alloc:
      new (&alloc) PointerAllocEffect(std::move(rhs.alloc));
      break;
    case PointerEffectType::Copy:
      new (&copy) PointerCopyEffect(std::move(rhs.copy));
      break;
    case PointerEffectType::Exit:
      new (&eexit) PointerExitEffect();
      break;
    }
  }

  // Move-assignment operator.
  PointerEffect &operator=(PointerEffect &&rhs) noexcept {
    if (this == &rhs)
      return *this;

    // Destroy the currently active union member.
    switch (type) {
    case PointerEffectType::Alloc:
      alloc.~PointerAllocEffect();
      break;
    case PointerEffectType::Copy:
      copy.~PointerCopyEffect();
      break;
    case PointerEffectType::Exit:
      eexit.~PointerExitEffect();
      break;
    }

    // Move-construct the new active member.
    type = rhs.type;
    switch (type) {
    case PointerEffectType::Alloc:
      new (&alloc) PointerAllocEffect(std::move(rhs.alloc));
      break;
    case PointerEffectType::Copy:
      new (&copy) PointerCopyEffect(std::move(rhs.copy));
      break;
    case PointerEffectType::Exit:
      new (&eexit) PointerExitEffect();
      break;
    }
    return *this;
  }

  PointerEffectType getType() const { return type; }

  const PointerAllocEffect &getAsAllocEffect() const {
    assert(type == PointerEffectType::Alloc);
    return alloc;
  }

  const PointerCopyEffect &getAsCopyEffect() const {
    assert(type == PointerEffectType::Copy);
    return copy;
  }

  static PointerEffect getAllocEffect() { return PointerEffect(); }

  static PointerEffect getAllocEffect(const APosition &sizePos) {
    return PointerEffect(sizePos);
  }

  static PointerEffect getCopyEffect(const CopyDest &d, const CopySource &s) {
    return PointerEffect(d, s);
  }

  static PointerEffect getExitEffect() {
    return PointerEffect(PointerExitEffect());
  }

  ~PointerEffect() {
    switch (type) {
    case PointerEffectType::Alloc:
      alloc.~PointerAllocEffect();
      break;
    case PointerEffectType::Copy:
      copy.~PointerCopyEffect();
      break;
    case PointerEffectType::Exit:
      eexit.~PointerExitEffect();
      break;
    }
  }
};

} // namespace annotation
