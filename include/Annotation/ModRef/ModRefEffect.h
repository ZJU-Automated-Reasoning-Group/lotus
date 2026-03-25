#pragma once

#include "Annotation/ArgPosition.h"

namespace annotation {

enum class ModRefType : bool { Mod, Ref };

enum class ModRefClass : bool { DirectMemory, ReachableMemory };

class ModRefEffect {
private:
  ModRefType type;
  ModRefClass mClass;
  APosition pos;

public:
  ModRefEffect(ModRefType t, ModRefClass c, const APosition &p)
      : type(t), mClass(c), pos(p) {}

  ModRefType getType() const { return type; }
  ModRefClass getClass() const { return mClass; }
  APosition getPosition() const { return pos; }

  bool isModEffect() const { return type == ModRefType::Mod; }
  bool isRefEffect() const { return type == ModRefType::Ref; }
  bool onDirectMemory() const { return mClass == ModRefClass::DirectMemory; }
  bool onReachableMemory() const {
    return mClass == ModRefClass::ReachableMemory;
  }
};

} // namespace annotation
