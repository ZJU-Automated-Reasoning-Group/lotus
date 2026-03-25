#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Type.h>

namespace tpa {

class TypeLayout;

enum class CFGNodeTag : std::uint8_t {
  Entry,
  Alloc,
  Copy,
  Offset,
  Load,
  Store,
  Call,
  Ret,
};

template <typename NodeType> class EntryNodeMixin : public NodeType {
public:
  EntryNodeMixin() : NodeType(CFGNodeTag::Entry) {}
};

template <typename NodeType> class AllocNodeMixin : public NodeType {
private:
  const llvm::Instruction *dest;
  const TypeLayout *typeLayout;

public:
  AllocNodeMixin(const llvm::Instruction *d, const TypeLayout *t)
      : NodeType(CFGNodeTag::Alloc), dest(d), typeLayout(t) {
    assert(d != nullptr && t != nullptr);
    assert(d->getType()->isPointerTy());
  }

  const llvm::Instruction *getDest() const { return dest; }
  const TypeLayout *getAllocTypeLayout() const { return typeLayout; }
};

template <typename NodeType> class CopyNodeMixin : public NodeType {
private:
  const llvm::Value *dest;

  using SrcList = std::vector<const llvm::Value *>;
  SrcList srcs;

public:
  using const_iterator = SrcList::const_iterator;

  // n-operand copy
  CopyNodeMixin(const llvm::Value *d, SrcList &&s)
      : NodeType(CFGNodeTag::Copy), dest(d), srcs(std::move(s)) {
    assert(d != nullptr);
  }

  const llvm::Value *getDest() const { return dest; }
  const llvm::Value *getSrc(size_t idx) const {
    assert(idx < srcs.size());
    return srcs[idx];
  }
  unsigned getNumSrc() const { return srcs.size(); }

  void setSrc(SrcList &&s) { srcs = std::move(s); }

  const_iterator begin() const { return srcs.begin(); }
  const_iterator end() const { return srcs.end(); }
};

template <typename NodeType> class OffsetNodeMixin : public NodeType {
private:
  const llvm::Value *dest;
  const llvm::Value *src;
  size_t offset;
  bool arrayRef;

public:
  OffsetNodeMixin(const llvm::Value *d, const llvm::Value *s, size_t o, bool a)
      : NodeType(CFGNodeTag::Offset), dest(d), src(s), offset(o), arrayRef(a) {
    assert(dest != nullptr && src != nullptr);
  }

  const llvm::Value *getDest() const { return dest; }
  const llvm::Value *getSrc() const { return src; }
  size_t getOffset() const { return offset; }
  bool isArrayRef() const { return arrayRef; }

  void setSrc(const llvm::Value *s) {
    assert(s != nullptr);
    src = s;
  }
};

template <typename NodeType> class LoadNodeMixin : public NodeType {
private:
  const llvm::Value *dest;
  const llvm::Value *src;

public:
  LoadNodeMixin(const llvm::Value *d, const llvm::Value *s)
      : NodeType(CFGNodeTag::Load), dest(d), src(s) {
    assert(dest != nullptr && src != nullptr);
  }

  const llvm::Value *getDest() const { return dest; }
  const llvm::Value *getSrc() const { return src; }

  void setSrc(const llvm::Value *s) {
    assert(s != nullptr);
    src = s;
  }
};

template <typename NodeType> class StoreNodeMixin : public NodeType {
private:
  const llvm::Value *dest;
  const llvm::Value *src;

public:
  StoreNodeMixin(const llvm::Value *d, const llvm::Value *s)
      : NodeType(CFGNodeTag::Store), dest(d), src(s) {
    assert(dest != nullptr && src != nullptr);
  }

  const llvm::Value *getDest() const { return dest; }
  const llvm::Value *getSrc() const { return src; }

  void setDest(const llvm::Value *d) {
    assert(d != nullptr);
    dest = d;
  }
  void setSrc(const llvm::Value *s) {
    assert(s != nullptr);
    src = s;
  }
};

template <typename NodeType> class CallNodeMixin : public NodeType {
private:
  using ArgumentList = std::vector<const llvm::Value *>;

  const llvm::Instruction *callsite;
  const llvm::Value *funPtr;
  ArgumentList args;

public:
  using const_iterator = ArgumentList::const_iterator;

  CallNodeMixin(const llvm::Value *f, const llvm::Instruction *c)
      : NodeType(CFGNodeTag::Call), callsite(c), funPtr(f) {
    assert(funPtr != nullptr && callsite != nullptr);
  }

  const llvm::Instruction *getDest() const {
    if (callsite->getType()->isPointerTy())
      return callsite;
    else
      return nullptr;
  }

  const llvm::Value *getFunctionPointer() const { return funPtr; }
  const llvm::Instruction *getCallSite() const { return callsite; }

  void setFunctionPointer(const llvm::Value *f) {
    assert(f != nullptr);
    funPtr = f;
  }

  void addArgument(const llvm::Value *v) {
    assert(v != nullptr);
    args.push_back(v);
  }
  const llvm::Value *getArgument(unsigned idx) const {
    assert(idx < args.size());
    return args[idx];
  }
  unsigned getNumArgument() const { return args.size(); }

  void setArgument(unsigned idx, const llvm::Value *v) {
    assert(v != nullptr && idx < args.size());
    args[idx] = v;
  }

  const_iterator begin() const { return args.begin(); }
  const_iterator end() const { return args.end(); }
};

template <typename NodeType> class ReturnNodeMixin : public NodeType {
private:
  // ret can be NULL if the function returns a void type
  const llvm::Value *ret;

public:
  ReturnNodeMixin(const llvm::Value *r = nullptr)
      : NodeType(CFGNodeTag::Ret), ret(r) {}

  const llvm::Value *getReturnValue() const { return ret; }
  void setReturnValue(const llvm::Value *r) { ret = r; }
};

} // namespace tpa