/*
Static Single Information (SSI) IR

SSI additionally guarantees that
- Every definition of a variable dominates all its uses (SSA property)
- Every use of a variable post-dominates all its reaching definitions

Static Single Information (SSI) form = SSA + σ-functions
1. Start from SSA form.
2. Compute the iterated post-dominance frontier (analogous to dominance frontier) to decide where σ's are needed.
3. Insert σ-functions at each control-flow split whose successors are not in the same post-dom tree region.
4. Rename variables again to give unique names to σ results (mirrors SSA renaming).

 * 	Command-line options
 * 		-v: 		verbose mode
 * 		-set xxxx:	set what will be the initial points (x either 1 or 0)
 *			- 1st: exit of conditionals, downwards
 *			- 2nd: exit of conditionals, upwards
 *			- 3rd: uses, downwards
 *			- 4th: uses, upwards
 */

#ifndef SSIFY_H_
#define SSIFY_H_

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <set>
#include <string>

#define DEBUG_TYPE "SSIfy"

namespace llvm {

STATISTIC(NumPHIsCreated, "Number of SSIfy_phis created");
STATISTIC(NumSigmasCreated, "Number of SSIfy_sigmas created");
STATISTIC(NumCopiesCreated, "Number of SSIfy_copies created");
STATISTIC(NumPHIsDeleted, "Number of SSIfy_phis deleted");
STATISTIC(NumSigmasDeleted, "Number of SSIfy_sigmas deleted");
STATISTIC(NumCopiesDeleted, "Number of SSIfy_copies deleted");

// Forward declarations
class ProgramPoint;
class RenamingStack;
class Graph;
struct PostDominanceFrontier;

// SSIfy
struct SSIfy : public FunctionPass {
  static const std::string phiname;
  static const std::string signame;
  static const std::string copname;

  static char ID; // Pass identification, replacement for typeid
  const Function *F;
  const DominatorTreeWrapperPass *DTw;
  const DominatorTree *DTmap;
  PostDominatorTree *PDTmap;
  const DominanceFrontier *DFmap;
  PostDominanceFrontier *PDFmap;

  // Command-line options for program points
  bool flags[4];

  // This map associates variables with the set of new variables
  // that have been created for them.
  // Key is always an Instruction* stored as Value* for map generality.
  DenseMap<Value *, SmallPtrSet<Instruction *, 4>> versions;

  // Side-table sets that identify SSI-inserted nodes without relying on names.
  // Fix for bug #2: name-based classification is fragile.
  DenseSet<const Instruction *> ssiPhiSet;
  DenseSet<const Instruction *> ssiSigmaSet;
  DenseSet<const Instruction *> ssiCopySet;

  SSIfy()
      : FunctionPass(ID), F(nullptr), DTw(nullptr), DTmap(nullptr),
        PDTmap(nullptr), DFmap(nullptr), PDFmap(nullptr) {
    memset(flags, 0, 4 * sizeof(bool));
  }

  virtual bool runOnFunction(Function &F);

  /*
   * Determines what is the splitting strategy for the variable V
   * and calls the SSIfy functions in order
   *   - split
   *   - rename
   */
  void run(Instruction *V);

  /*
   * Splits live range of the variable V according to the splitting strategy
   * defined as input.
   */
  void split(Instruction *V, const std::set<ProgramPoint> &Iup,
             const std::set<ProgramPoint> &Idown);

  // Returns true when inserting an SSI node at insert_point for V would be
  // provably useless (no use of V or any of its existing versions is dominated
  // by insert_point).  Fix for bug #18: now also checks existing versions.
  bool isNotNecessary(const Instruction *insert_point, const Value *V) const;

  /*
   * Renaming function.
   * Called after the creation of new variables (split function).
   * Fix for bug #5: starts from the function entry block so that all uses
   * reachable from the entry are visited, not just those in the subtree
   * rooted at V's defining block.
   */
  void rename_initial(Instruction *V);

  /*
   * Passes through all instructions in BB to update uses of the
   * variable V to its most recent definition, as well as registering
   * new definitions when it takes place.
   * Fix for bug #6: stack depth is saved/restored around child recursion.
   */
  void rename(BasicBlock *BB, RenamingStack &stack);

  /*
   * Pushes into the stack a new definition of the variable V, that being the
   * instruction inst.
   */
  void set_def(RenamingStack &stack, Instruction *inst);

  /*
   * Renames uses of the variable V in the instruction inst to its last
   * definition according to the stack of definitions.
   *
   * Fix for bug #7: uses a read-only scan of the stack instead of
   * destructively popping entries.
   *
   * from stands for pointer to the predecessor block. It is used when renaming
   * variables inside a SSI_phi to tell which incoming value should be renamed.
   */
  void set_use(RenamingStack &stack, Instruction *inst,
               BasicBlock *from = nullptr);

  /*
   * Look at this->versions, which contains all new variables created and what
   * Value were they created for, and determines which ones should be removed
   * for not being useful or simply wrong.
   */
  void clean();

  /*
   * These functions check whether an instruction is of the custom types that
   * we create in this pass.
   * Fix for bug #2: checks the side-table sets, not instruction names.
   */
  bool is_SSIphi(const Instruction *I) const;
  bool is_SSIsigma(const Instruction *I) const;
  bool is_SSIcopy(const Instruction *I) const;

  /*
   * Checks if I is an actual instruction.
   * Actual instruction is defined as not being created by us,
   * that is, sigma, artificial phi, and copy.
   */
  bool is_actual(const Instruction *I) const;

  // For a given BasicBlock, return its iterated dominance frontier as a set.
  // Fix for bug #3: guards against missing entries in DFmap.
  SmallPtrSet<BasicBlock *, 8> get_iterated_df(BasicBlock *BB) const;

  // For a given BasicBlock, return its iterated post-dominance frontier as a
  // set.  Fix for bug #3: guards against missing entries in PDFmap.
  SmallPtrSet<BasicBlock *, 8> get_iterated_pdf(BasicBlock *BB) const;

  /*
   * Creates a topological sorting of instructions in to_be_erased,
   * based on relations from this->versions.
   * Fix for bug #8: uses a proper three-colour DFS with cycle detection.
   */
  SmallVector<Instruction *, 8>
  get_topsort_versions(const SmallPtrSet<Instruction *, 16> &to_be_erased) const;

  // Three-colour DFS visitor used by get_topsort_versions.
  // colour: 0 = white (unvisited), 1 = grey (in progress), 2 = black (done).
  void visit(Graph &g, DenseMap<Value *, int> &colour,
             SmallVectorImpl<Instruction *> &list, Value *V) const;

  void getAnalysisUsage(AnalysisUsage &AU) const;
};

/*
 * A program point is a pair of one instruction and a region.
 *
 * Region can be: in (entry of block, join point, phi insertion)
 *                self (middle of block, parallel copy insertion)
 *                out (exit of block, branch point, sigma insertion)
 *
 * Instructions have different uses depending on the region associated
 *       in:   instruction is used only to determine what BasicBlock the
 *             program point refers to.
 *       self: instruction is the precise insertion point of the parallel
 *             copy. Thus, the future copy will be inserted just after the
 *             instruction here.
 *       out:  instruction is the branch instruction that is followed by the
 *             two outgoing edges.
 */
class ProgramPoint {
public:
  typedef enum { In, Self, Out } Position;

public:
  explicit ProgramPoint(Instruction *I, Position P);

  /*
   * Checks if this program point doesn't have a definition of V already.
   * We have three cases: sigma, phi, or copy.
   * Each one has a different logic.
   */
  bool not_definition_of(const Value *V, const SSIfy &pass) const;

  /*
   * These are used to differentiate program points.
   * Since we store them in sets, these are useful
   * to implement the prevention of duplication, as well as ordering.
   */
  bool operator==(const ProgramPoint &o) const;
  bool operator!=(const ProgramPoint &o) const;
  bool operator<(const ProgramPoint &o) const;
  bool operator>(const ProgramPoint &o) const;

  /*
   * Return the type of a program point.
   */
  bool is_join() const;
  bool is_branch() const;
  bool is_copy() const;

  Instruction *I;
  Position P;
};

/*
 * Wrapper class for the stack, in order to store the Value for which
 * this stack exists in the first place.
 */
class RenamingStack {
public:
  SmallVector<Instruction *, 4> stack;
  Value *V;

public:
  RenamingStack(Value *V);
  Value *getValue() const;
  void push(Instruction *I);
  void pop();
  Instruction *peek() const;
  bool empty() const;
  unsigned size() const;
  void resize(unsigned n);
};

/*
 * Used to determine topological ordering in the clean function.
 */
class Graph {
public:
  // Map from Values to adjacency lists
  DenseMap<Value *, SmallPtrSet<Value *, 4>> vertices;

  void addNode(Value *V);
  bool hasNode(Value *V);

  /*
   * Add edge to graph.
   * If from is not in the graph, we do not add it!
   */
  void addEdge(Value *from, Value *to);

  /*
   * Return if an edge is present in the graph.
   * We do not create any new nodes.
   */
  bool hasEdge(Value *from, Value *to);
};

/// PostDominanceFrontier Class - Concrete subclass of DominanceFrontier that is
/// used to compute the post-dominance frontier.
///
/// Fix for bug #4: calculate_frontiers now correctly populates Roots and
/// recurses through the entire post-dominator tree so that the frontier map
/// is fully built before any query is made.
struct PostDominanceFrontier {
  static char ID;

public:
  typedef std::set<BasicBlock *> DomSetType;    // Dom set for a bb
  typedef std::map<BasicBlock *, DomSetType> DomSetMapType; // Dom set map

  DomSetMapType Frontiers;
  std::vector<BasicBlock *> Roots;
  const bool IsPostDominators;

  explicit PostDominanceFrontier(PostDominatorTree *PDT)
      : IsPostDominators(true) {
    calculate_frontiers(PDT);
  }

  virtual ~PostDominanceFrontier() { releaseMemory(); }

  /// getRoots - Return the root blocks of the current CFG.
  inline const std::vector<BasicBlock *> &getRoots() const { return Roots; }

  bool isPostDominator() const { return IsPostDominators; }

  virtual void releaseMemory() { Frontiers.clear(); }

  // Accessor interface:
  typedef DomSetMapType::iterator iterator;
  typedef DomSetMapType::const_iterator const_iterator;
  iterator begin() { return Frontiers.begin(); }
  const_iterator begin() const { return Frontiers.begin(); }
  iterator end() { return Frontiers.end(); }
  const_iterator end() const { return Frontiers.end(); }
  iterator find(BasicBlock *B) { return Frontiers.find(B); }
  const_iterator find(BasicBlock *B) const { return Frontiers.find(B); }

  iterator addBasicBlock(BasicBlock *BB, const DomSetType &frontier) {
    assert(find(BB) == end() && "Block already in DominanceFrontier!");
    return Frontiers.insert(std::make_pair(BB, frontier)).first;
  }

  void removeBlock(BasicBlock *BB) {
    assert(find(BB) != end() && "Block is not in DominanceFrontier!");
    for (iterator I = begin(), E = end(); I != E; ++I)
      I->second.erase(BB);
    Frontiers.erase(BB);
  }

  void addToFrontier(iterator I, BasicBlock *Node) {
    assert(I != end() && "BB is not in DominanceFrontier!");
    I->second.insert(Node);
  }

  void removeFromFrontier(iterator I, BasicBlock *Node) {
    assert(I != end() && "BB is not in DominanceFrontier!");
    assert(I->second.count(Node) && "Node is not in DominanceFrontier of BB");
    I->second.erase(Node);
  }

  bool compareDomSet(DomSetType &DS1, const DomSetType &DS2) const {
    std::set<BasicBlock *> tmpSet;
    for (DomSetType::const_iterator I = DS2.begin(), E = DS2.end(); I != E;
         ++I)
      tmpSet.insert(*I);

    for (DomSetType::const_iterator I = DS1.begin(), E = DS1.end(); I != E;) {
      BasicBlock *Node = *I++;
      if (tmpSet.erase(Node) == 0)
        return true;
    }

    if (!tmpSet.empty())
      return true;

    return false;
  }

  bool compare(DominanceFrontier &Other) const {
    DomSetMapType tmpFrontiers;
    for (DomSetMapType::const_iterator I = Other.begin(), E = Other.end();
         I != E; ++I)
      tmpFrontiers.insert(std::make_pair(I->first, I->second));

    for (DomSetMapType::iterator I = tmpFrontiers.begin(),
                                 E = tmpFrontiers.end();
         I != E;) {
      BasicBlock *Node = I->first;
      const_iterator DFI = find(Node);
      if (DFI == end())
        return true;

      if (compareDomSet(I->second, DFI->second))
        return true;

      ++I;
      tmpFrontiers.erase(Node);
    }

    if (!tmpFrontiers.empty())
      return true;

    return false;
  }

  // Fix for bug #4: properly populate Roots and recurse through the full
  // post-dominator tree so that every block's frontier is computed.
  bool calculate_frontiers(PostDominatorTree *PDT) {
    Frontiers.clear();
    Roots.clear();

    if (!PDT)
      return false;

    const DomTreeNode *Root = PDT->getRootNode();
    if (!Root)
      return false;

    // The virtual root of the post-dominator tree may have a null block
    // (representing the "exit" virtual node).  Collect the real exit blocks
    // (children of the virtual root whose block is non-null) as Roots.
    if (Root->getBlock() == nullptr) {
      // Virtual root: its children are the actual exit blocks.
      for (DomTreeNode::const_iterator CI = Root->begin(), CE = Root->end();
           CI != CE; ++CI) {
        if (BasicBlock *BB = (*CI)->getBlock())
          Roots.push_back(BB);
      }
    } else {
      Roots.push_back(Root->getBlock());
    }

    // Recurse through the entire post-dominator tree.
    calculate(*PDT, Root);
    return false;
  }

private:
  // Public entry point: dispatches to calculateVirtualRoot or calculateNode.
  const DomSetType &calculate(const PostDominatorTree &DT,
                               const DomTreeNode *Node);

  // Fix for bug G: split into two helpers to avoid returning a reference to
  // a shared mutable static object for the virtual-root case.

  // Handles the virtual root (null block): recurses into real children.
  void calculateVirtualRoot(const PostDominatorTree &DT,
                             const DomTreeNode *Node);

  // Computes and stores the frontier for a real (non-null-block) node.
  const DomSetType &calculateNode(const PostDominatorTree &DT,
                                   const DomTreeNode *Node);
};

} // namespace llvm

#endif /* SSIFY_H_ */
