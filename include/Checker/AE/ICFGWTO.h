#pragma once

#include <list>
#include <map>
#include <set>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>

namespace lotus {
namespace analysis {

class ICFGWTO;
class ICFGSingletonWTO;
class ICFGCycleWTO;

class ICFGWTOComp {
public:
  enum Kind { Singleton, Cycle };

private:
  Kind kind;

protected:
  ICFGWTOComp(Kind k) : kind(k) {}

public:
  virtual ~ICFGWTOComp() = default;
  Kind getKind() const { return kind; }
  static bool classof(const ICFGWTOComp *c) {
    (void)c;
    return true;
  }
  virtual std::vector<const llvm::BasicBlock *> getSuccessors() const = 0;
};

class ICFGSingletonWTO : public ICFGWTOComp {
private:
  const llvm::BasicBlock *bb;

public:
  explicit ICFGSingletonWTO(const llvm::BasicBlock *b)
      : ICFGWTOComp(Singleton), bb(b) {}
  const llvm::BasicBlock *getBlock() const { return bb; }
  std::vector<const llvm::BasicBlock *> getSuccessors() const override;
  static bool classof(const ICFGWTOComp *c) {
    return c->getKind() == Singleton;
  }
};

class ICFGCycleWTO : public ICFGWTOComp {
private:
  const llvm::BasicBlock *entry;
  std::list<const ICFGWTOComp *> components;

public:
  explicit ICFGCycleWTO(const llvm::BasicBlock *e)
      : ICFGWTOComp(Cycle), entry(e) {}
  const llvm::BasicBlock *getEntry() const { return entry; }
  void addComponent(const ICFGWTOComp *comp) { components.push_back(comp); }
  const std::list<const ICFGWTOComp *> &getComponents() const {
    return components;
  }
  std::vector<const llvm::BasicBlock *> getSuccessors() const override;
  std::vector<const llvm::BasicBlock *>
  getExitSuccessors(const llvm::BasicBlock *exitBB) const;
  static bool classof(const ICFGWTOComp *c) { return c->getKind() == Cycle; }
};

class ICFGWTO {
private:
  const llvm::Function *func;
  const llvm::BasicBlock *entry;
  std::list<const ICFGWTOComp *> components;

public:
  explicit ICFGWTO(const llvm::Function *f);
  const llvm::Function *getFunction() const { return func; }
  const llvm::BasicBlock *getEntry() const { return entry; }
  const std::list<const ICFGWTOComp *> &getComponents() const {
    return components;
  }

  std::vector<const llvm::BasicBlock *>
  getSuccessors(const llvm::BasicBlock *bb) const;
  std::vector<const llvm::BasicBlock *> getNodes() const;

private:
  void buildWTO();
  const ICFGWTOComp *
  buildComponent(const llvm::BasicBlock *bb,
                 std::set<const llvm::BasicBlock *> &visited,
                 std::set<const llvm::BasicBlock *> &inStack);
};

} // namespace analysis
} // namespace lotus
