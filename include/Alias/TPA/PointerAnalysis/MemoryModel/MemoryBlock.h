#pragma once

#include "Alias/TPA/PointerAnalysis/MemoryModel/AllocSite.h"

namespace tpa {

class TypeLayout;

// MemoryBlock is a block of memory consists of potentially many memory objects.
// It is the unit of memory allocation
class MemoryBlock {
private:
  AllocSite allocSite;
  const TypeLayout *type;

  MemoryBlock(const AllocSite &a, const TypeLayout *ty)
      : allocSite(a), type(ty) {}

public:
  const AllocSite &getAllocSite() const { return allocSite; }
  const TypeLayout *getTypeLayout() const { return type; }

  bool isNullBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Null;
  }
  bool isUniversalBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Universal;
  }
  bool isGlobalBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Global ||
           allocSite.getAllocType() == AllocSiteTag::Null ||
           allocSite.getAllocType() == AllocSiteTag::Universal;
  }
  bool isFunctionBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Function;
  }
  bool isStackBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Stack;
  }
  bool isHeapBlock() const {
    return allocSite.getAllocType() == AllocSiteTag::Heap;
  }

  friend class MemoryManager;
};

} // namespace tpa