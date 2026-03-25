//===- SSITransform.cpp - SSI transformation logic ------------------------===//
//
//    This file is licensed under the General Public License v2.
//
// Author: rainoftime
//===----------------------------------------------------------------------===//
/*
 *   Core SSI transformation algorithms: split, rename, and clean.
 *
 * Bugs fixed in this file:
 *   #1  - startswith → starts_with (LLVM 14 deprecation)
 *   #2  - SSI node classification uses side-table sets, not name prefixes
 *   #3  - get_iterated_df / get_iterated_pdf guard against missing map entries
 *   #5  - rename_initial starts from the function entry, not V's defining block
 *   #6  - rename() saves/restores stack depth around dominator-tree child
 * recursion #7  - set_use() scans the stack read-only instead of destructively
 * popping #8  - visit() uses three-colour DFS to detect and skip cycles #9  -
 * clean() verifies that V dominates use sites before replacing with V #18 -
 * isNotNecessary() also checks existing SSI versions of V
 */

#include "IR/SSI/SSI.h"

using namespace llvm;

extern cl::opt<bool> Verbose;

// ---------------------------------------------------------------------------
// split
// ---------------------------------------------------------------------------

void SSIfy::split(Instruction *V, const std::set<ProgramPoint> &Iup,
                  const std::set<ProgramPoint> &Idown) {
  std::set<ProgramPoint> Sup;
  std::set<ProgramPoint> Sdown;

  if (Verbose) {
    errs() << "Splitting " << V->getName() << "\n";
  }

  // Creation of the Sup set. Its logic is defined in the referenced paper.
  for (const ProgramPoint &point : Iup) {
    Instruction *I = point.I;
    BasicBlock *BBparent = I->getParent();

    if (point.is_join()) {
      for (pred_iterator PI = pred_begin(BBparent), E = pred_end(BBparent);
           PI != E; ++PI) {
        BasicBlock *BBpred = *PI;
        SmallPtrSet<BasicBlock *, 8> iterated_pdf = get_iterated_pdf(BBpred);
        for (BasicBlock *BB : iterated_pdf) {
          Instruction &last = BB->back();
          Sup.insert(ProgramPoint(&last, ProgramPoint::Out));
        }
      }
    } else {
      SmallPtrSet<BasicBlock *, 8> iterated_pdf = get_iterated_pdf(BBparent);
      for (BasicBlock *BB : iterated_pdf) {
        Instruction &last = BB->back();
        Sup.insert(ProgramPoint(&last, ProgramPoint::Out));
      }
    }
  }

  // Union of Sup and Idown to form the seed set for Sdown.
  std::set<ProgramPoint> NewSet;
  NewSet.insert(Sup.begin(), Sup.end());
  NewSet.insert(Idown.begin(), Idown.end());

  // Creation of Sdown. Logic defined in the paper as well.
  for (const ProgramPoint &point : NewSet) {
    Instruction *I = point.I;
    BasicBlock *BBparent = I->getParent();

    if (point.is_branch()) {
      for (succ_iterator PI = succ_begin(BBparent), E = succ_end(BBparent);
           PI != E; ++PI) {
        BasicBlock *BBsucc = *PI;
        SmallPtrSet<BasicBlock *, 8> iterated_df = get_iterated_df(BBsucc);
        for (BasicBlock *BB : iterated_df) {
          Instruction &first = BB->front();
          Sdown.insert(ProgramPoint(&first, ProgramPoint::In));
        }
      }
    } else {
      SmallPtrSet<BasicBlock *, 8> iterated_df = get_iterated_df(BBparent);
      for (BasicBlock *BB : iterated_df) {
        Instruction &first = BB->front();
        Sdown.insert(ProgramPoint(&first, ProgramPoint::In));
      }
    }
  }

  // Final set S = Iup ∪ Idown ∪ Sup ∪ Sdown
  std::set<ProgramPoint> S;
  S.insert(Iup.begin(), Iup.end());
  S.insert(Idown.begin(), Idown.end());
  S.insert(Sup.begin(), Sup.end());
  S.insert(Sdown.begin(), Sdown.end());

  // Split live range of V by inserting sigma, phi, and copies.
  for (const ProgramPoint &point : S) {
    if (point.not_definition_of(V, *this)) {
      Instruction *insertion_point = point.I;
      ProgramPoint::Position relative_position = point.P;

      // Check if new variable is actually not necessary.
      // Fix for bug #18: isNotNecessary now also checks existing versions.
      if (isNotNecessary(insertion_point, V)) {
        continue;
      }

      if (point.is_join()) {
        // phi
        unsigned numReservedValues =
            std::distance(pred_begin(insertion_point->getParent()),
                          pred_end(insertion_point->getParent()));
        PHINode *new_phi =
            PHINode::Create(V->getType(), numReservedValues, phiname);

        // Add V as incoming value from every predecessor.
        for (pred_iterator BBit = pred_begin(insertion_point->getParent()),
                           BBend = pred_end(insertion_point->getParent());
             BBit != BBend; ++BBit) {
          BasicBlock *predBB = *BBit;
          new_phi->addIncoming(V, predBB);
        }

        switch (relative_position) {
        case ProgramPoint::In:
          new_phi->insertBefore(insertion_point);
          break;
        default:
          errs() << "Problem here1";
          break;
        }

        if (Verbose) {
          errs() << "Created " << new_phi->getName() << "\n";
        }

        // Fix for bug #2: register in side-table, not just by name.
        this->ssiPhiSet.insert(new_phi);
        this->versions[V].insert(new_phi);
        ++NumPHIsCreated;

      } else if (point.is_branch()) {
        // sigma — one per successor of the branch block.
        BasicBlock *BBparent = point.I->getParent();
        unsigned numReservedValues = 1;

        for (succ_iterator PI = succ_begin(BBparent), E = succ_end(BBparent);
             PI != E; ++PI) {
          BasicBlock *BBsucc = *PI;

          PHINode *new_sigma = PHINode::Create(V->getType(), numReservedValues,
                                               signame, &BBsucc->front());
          new_sigma->addIncoming(V, BBparent);

          if (Verbose) {
            errs() << "Created " << new_sigma->getName() << "\n";
          }

          // Fix for bug #2: register in side-table.
          this->ssiSigmaSet.insert(new_sigma);
          this->versions[V].insert(new_sigma);
          ++NumSigmasCreated;
        }

      } else if (point.is_copy()) {
        // copy — represented as "V + 0" so it is a distinct SSA value.
        ConstantInt *zero =
            ConstantInt::get(cast<IntegerType>(V->getType()), 0);

        BinaryOperator *new_copy =
            BinaryOperator::Create(Instruction::Add, V, zero, copname);

        switch (relative_position) {
        case ProgramPoint::Self:
          new_copy->insertAfter(insertion_point);
          break;
        default:
          errs() << "Problem here2";
          break;
        }

        if (Verbose) {
          errs() << "Created " << new_copy->getName() << "\n";
        }

        // Fix for bug #2: register in side-table.
        this->ssiCopySet.insert(new_copy);
        this->versions[V].insert(new_copy);
        ++NumCopiesCreated;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// rename_initial / rename
// ---------------------------------------------------------------------------

void SSIfy::rename_initial(Instruction *V) {
  RenamingStack stack(V);

  // Fix for bug #5: start renaming from the function entry block, not from
  // V's defining block.  The standard SSA renaming algorithm walks the full
  // dominator tree from the root so that every reachable use is visited.
  // Starting from V's block misses uses in blocks that dominate V's block
  // (which can arise for SSI-inserted nodes) and is incorrect in general.
  BasicBlock *entry = &V->getParent()->getParent()->getEntryBlock();
  rename(entry, stack);
}

void SSIfy::rename(BasicBlock *BB, RenamingStack &stack) {
  const Value *V = stack.getValue();

  if (Verbose) {
    errs() << "Renaming " << V->getName() << " in " << BB->getName() << "\n";
  }

  // Fix for bug #6: record the stack depth on entry so we can restore it
  // after processing this block's dominator-tree children.
  unsigned stackDepthOnEntry = stack.size();

  // Iterate over all instructions in BB.
  for (BasicBlock::iterator iit = BB->begin(), iend = BB->end(); iit != iend;
       ++iit) {
    Instruction *I = cast<Instruction>(&*iit);
    PHINode *phi = dyn_cast<PHINode>(I);

    bool has_newdef = false;

    // Check if I has a use of V.
    // If it does, mark I as a new definition of V and call set_def later.
    for (User::op_iterator i = I->op_begin(), e = I->op_end(); i != e; ++i) {
      Value *used = *i;

      if (used == V) {
        if (!is_actual(I)) {
          has_newdef = true;
        }

        if (!is_SSIphi(I)) {
          set_use(stack, I);
        }

        break;
      }
    }

    // NEW DEFINITION OF V: sigma, phi, or copy.
    if (has_newdef) {
      if (phi) {
        set_def(stack, phi);
      } else if (is_SSIcopy(I)) {
        set_def(stack, I);
      }
    }
  }

  // Rename uses of V inside SSI_phis in successor blocks.
  for (succ_iterator sit = succ_begin(BB), send = succ_end(BB); sit != send;
       ++sit) {
    BasicBlock *BBsucc = *sit;
    for (BasicBlock::iterator BBit = BBsucc->begin(),
                              BBend = BBsucc->getFirstInsertionPt();
         BBit != BBend; ++BBit) {
      PHINode *phi = dyn_cast<PHINode>(&*BBit);

      if (phi && is_SSIphi(phi)) {
        set_use(stack, phi, BB);
      }
    }
  }

  // Recurse into dominator-tree children.
  DomTreeNode *domtree = this->DTmap->getNode(BB);
  if (domtree) {
    for (DomTreeNode::iterator begin = domtree->begin(), end = domtree->end();
         begin != end; ++begin) {
      DomTreeNodeBase<BasicBlock> *DTN_children = *begin;
      BasicBlock *BB_children = DTN_children->getBlock();
      rename(BB_children, stack);
    }
  }

  // Fix for bug #6: restore the stack to the depth it had when we entered
  // this block.  Definitions pushed while processing this block are only
  // valid within its dominator-tree subtree and must not be visible to
  // sibling subtrees.
  stack.resize(stackDepthOnEntry);
}

// ---------------------------------------------------------------------------
// set_use / set_def
// ---------------------------------------------------------------------------

void SSIfy::set_use(RenamingStack &stack, Instruction *inst, BasicBlock *from) {
  Value *V = stack.getValue();

  // If the stack is empty, renaming hasn't reached the initial definition of
  // V yet, so there is nothing to rename.
  if (stack.empty()) {
    return;
  }

  // Fix for bug #7: find the correct dominating definition by scanning the
  // stack from top to bottom WITHOUT popping entries.  The original code
  // destructively popped entries, which corrupted the stack for subsequent
  // uses in the same dominator-tree subtree.
  Instruction *new_name = nullptr;

  if (!from) {
    // Normal use: find the topmost stack entry that dominates inst.
    for (int idx = (int)stack.size() - 1; idx >= 0; --idx) {
      Instruction *candidate = stack.stack[idx];
      if (this->DTmap->dominates(candidate, inst)) {
        new_name = candidate;
        break;
      }
    }
  } else {
    // SSI_phi incoming-value renaming: find the topmost stack entry that
    // either lives in 'from' or dominates 'from'.
    for (int idx = (int)stack.size() - 1; idx >= 0; --idx) {
      Instruction *candidate = stack.stack[idx];
      if (candidate->getParent() == from ||
          this->DTmap->dominates(candidate, from)) {
        new_name = candidate;
        break;
      }
    }
  }

  // If no dominating definition was found, V itself is the reaching def.
  if (!new_name) {
    new_name = cast<Instruction>(V);
  }

  // Perform the renaming only when it would actually change something.
  if ((new_name != V) && (new_name != inst)) {
    if (!from) {
      if (Verbose) {
        errs() << "set_use: Renaming uses of " << V->getName() << " in "
               << inst->getName() << " to " << new_name->getName() << "\n";
      }
      inst->replaceUsesOfWith(V, new_name);
    } else {
      PHINode *phi = cast<PHINode>(inst);
      int index = phi->getBasicBlockIndex(from);

      if (index >= 0 && phi->getIncomingValue(index) == V) {
        if (Verbose) {
          errs() << "set_usephi: Renaming uses of " << V->getName() << " in "
                 << inst->getName() << " to " << new_name->getName() << "\n";
        }
        phi->setIncomingValue(index, new_name);
      }
    }
  }
}

void SSIfy::set_def(RenamingStack &stack, Instruction *inst) {
  if (Verbose) {
    errs() << "set_def: Pushing " << inst->getName() << " to the stack of "
           << stack.getValue()->getName() << "\n";
  }
  stack.push(inst);
}

// ---------------------------------------------------------------------------
// clean
// ---------------------------------------------------------------------------

void SSIfy::clean() {
  /*
   This structure saves all instructions that are marked to be erased.
   We cannot simply erase on sight because of cases like this:
     [V] -> {A B C D}
     [B] -> {...}
   If we visit V's set first and then erase B, the next iteration
   would try to access B, which would have been already erased.
   Thus, erases are performed afterwards.
  */
  SmallPtrSet<Instruction *, 16> to_be_erased;

  // This map associates instructions (that will be removed) to the Values
  // to which their uses will be renamed.
  DenseMap<Instruction *, Instruction *> maptooldvalues;

  for (auto &kv : this->versions) {
    Instruction *V = cast<Instruction>(kv.first);
    SmallPtrSet<Instruction *, 4> &created_vars = kv.second;

    for (Instruction *newvar : created_vars) {
      if (is_SSIphi(newvar)) {
        PHINode *ssi_phi = cast<PHINode>(newvar);
        bool any_value_diff_V = false;

        // Case 1: all incoming values are V itself — the phi is trivial.
        for (unsigned i = 0, n = ssi_phi->getNumIncomingValues(); i < n; ++i) {
          if (ssi_phi->getIncomingValue(i) != V) {
            any_value_diff_V = true;
            break;
          }
        }

        if (!any_value_diff_V) {
          if (Verbose)
            errs() << "Erasing " << ssi_phi->getName() << "\n";
          to_be_erased.insert(ssi_phi);
          maptooldvalues[ssi_phi] = V;
          continue;
        }

        // Case 2: phi is not dominated by V — it is unreachable / wrong.
        if (!this->DTmap->dominates(V, ssi_phi)) {
          if (Verbose)
            errs() << "Erasing " << ssi_phi->getName() << "\n";
          to_be_erased.insert(ssi_phi);
          maptooldvalues[ssi_phi] = V;
          continue;
        }

        // Case 3: phi has no uses.
        if (ssi_phi->use_empty()) {
          if (Verbose)
            errs() << "Erasing " << ssi_phi->getName() << "\n";
          to_be_erased.insert(ssi_phi);
          maptooldvalues[ssi_phi] = V;
          continue;
        }

      } else if (is_SSIsigma(newvar) || is_SSIcopy(newvar)) {
        if (newvar->use_empty()) {
          if (Verbose)
            errs() << "Erasing " << newvar->getName() << "\n";
          to_be_erased.insert(newvar);
          // No uses to remap, so no entry in maptooldvalues needed.

        } else if (!this->DTmap->dominates(V, newvar)) {
          if (Verbose)
            errs() << "Erasing " << newvar->getName() << "\n";

          // Fix for bug #9: only replace uses with V when V actually
          // dominates every use site of newvar.  If it does not, replacing
          // with V would produce IR that violates SSA dominance.
          // Fix for new bug E: only add to to_be_erased when we can safely
          // remap all uses (or there are no uses), to avoid erasing an
          // instruction that still has live uses with no replacement.
          bool v_dominates_all_uses = true;
          for (User *U : newvar->users()) {
            // Fix for bug F: a user might not be an Instruction (e.g. a
            // ConstantExpr); use dyn_cast and skip non-instruction users.
            Instruction *use_inst = dyn_cast<Instruction>(U);
            if (!use_inst || !this->DTmap->dominates(V, use_inst)) {
              v_dominates_all_uses = false;
              break;
            }
          }

          if (v_dominates_all_uses) {
            to_be_erased.insert(newvar);
            maptooldvalues[newvar] = V;
          }
          // If V does not dominate all uses, we cannot safely remove newvar.
          // Leave it in place; it will either be cleaned up by a later DCE
          // pass or flagged by the IR verifier as a structural problem.
        }
      } else {
        errs() << "Problem here3\n";
      }
    }
  }

  // Topological sort so that we erase in dependency order.
  SmallVector<Instruction *, 8> topsort = get_topsort_versions(to_be_erased);

  for (Instruction *I : topsort) {
    auto it = maptooldvalues.find(I);
    if (it != maptooldvalues.end()) {
      I->replaceAllUsesWith(it->second);
    }

    // STATISTICS
    if (is_SSIphi(I)) {
      ++NumPHIsDeleted;
    } else if (is_SSIsigma(I)) {
      ++NumSigmasDeleted;
    } else if (is_SSIcopy(I)) {
      ++NumCopiesDeleted;
    }

    // Remove from side-table sets before erasing.
    ssiPhiSet.erase(I);
    ssiSigmaSet.erase(I);
    ssiCopySet.erase(I);

    I->eraseFromParent();
  }
}

// ---------------------------------------------------------------------------
// SSI node classification — fix for bug #2
// ---------------------------------------------------------------------------

// Fix for bug #2: use side-table sets for classification.
// The name-based approach (startswith) is fragile: any user variable whose
// LLVM IR name happens to start with "SSIfy_phi" etc. would be misclassified.
// We keep the name-based fallback only for the static (const) overloads that
// are called from ProgramPoint::not_definition_of before the pass object is
// available; those are only used during the split phase before any renaming
// has occurred, so false positives are unlikely in practice.

bool SSIfy::is_SSIphi(const Instruction *I) const {
  return ssiPhiSet.count(I) > 0;
}

bool SSIfy::is_SSIsigma(const Instruction *I) const {
  return ssiSigmaSet.count(I) > 0;
}

bool SSIfy::is_SSIcopy(const Instruction *I) const {
  return ssiCopySet.count(I) > 0;
}

bool SSIfy::is_actual(const Instruction *I) const {
  return !is_SSIphi(I) && !is_SSIsigma(I) && !is_SSIcopy(I);
}

// ---------------------------------------------------------------------------
// get_iterated_df / get_iterated_pdf — fix for bug #3
// ---------------------------------------------------------------------------

SmallPtrSet<BasicBlock *, 8> SSIfy::get_iterated_df(BasicBlock *BB) const {
  SmallPtrSet<BasicBlock *, 8> iterated_df;
  SmallVector<BasicBlock *, 8> worklist;
  worklist.push_back(BB);

  while (!worklist.empty()) {
    BasicBlock *current = worklist.pop_back_val();

    // Fix for bug #3: guard against blocks not present in the frontier map
    // (e.g. unreachable blocks or blocks added during the transformation).
    auto it = this->DFmap->find(current);
    if (it == this->DFmap->end())
      continue;

    const DominanceFrontier::DomSetType &frontier = it->second;

    for (BasicBlock *BB_infrontier : frontier) {
      if (iterated_df.insert(BB_infrontier).second) {
        worklist.push_back(BB_infrontier);
      }
    }
  }

  return iterated_df;
}

SmallPtrSet<BasicBlock *, 8> SSIfy::get_iterated_pdf(BasicBlock *BB) const {
  SmallPtrSet<BasicBlock *, 8> iterated_pdf;
  SmallVector<BasicBlock *, 8> worklist;
  worklist.push_back(BB);

  while (!worklist.empty()) {
    BasicBlock *current = worklist.pop_back_val();

    // Fix for bug #3: guard against blocks not present in the PDF map.
    auto it = this->PDFmap->find(current);
    if (it == this->PDFmap->end())
      continue;

    const PostDominanceFrontier::DomSetType &frontier = it->second;

    for (BasicBlock *BB_infrontier : frontier) {
      if (iterated_pdf.insert(BB_infrontier).second) {
        worklist.push_back(BB_infrontier);
      }
    }
  }

  return iterated_pdf;
}

// ---------------------------------------------------------------------------
// isNotNecessary — fix for bug #18
// ---------------------------------------------------------------------------

bool SSIfy::isNotNecessary(const Instruction *insert_point,
                           const Value *V) const {
  // Check uses of V itself.
  for (const User *U : V->users()) {
    // Fix for bug F: users of a Value are not always Instructions (e.g. a
    // ConstantExpr can use a Value).  Skip non-instruction users to avoid
    // a crash in cast<Instruction>.
    const Instruction *use = dyn_cast<Instruction>(U);
    if (!use)
      continue;
    if (this->DTmap->dominates(insert_point, use)) {
      return false;
    }
  }

  // Fix for bug #18: also check uses of existing SSI versions of V.
  // The original code only checked uses of V, so it could incorrectly skip
  // an insertion point that is needed to rename a use of an already-created
  // sigma or phi.
  auto mit = this->versions.find(const_cast<Value *>(V));
  if (mit != this->versions.end()) {
    for (const Instruction *ver : mit->second) {
      for (const User *U : ver->users()) {
        // Same non-instruction-user guard as above.
        const Instruction *use = dyn_cast<Instruction>(U);
        if (!use)
          continue;
        if (this->DTmap->dominates(insert_point, use)) {
          return false;
        }
      }
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// Topological sort — fix for bug #8
// ---------------------------------------------------------------------------

SmallVector<Instruction *, 8> SSIfy::get_topsort_versions(
    const SmallPtrSet<Instruction *, 16> &to_be_erased) const {
  SmallVector<Instruction *, 8> topsort;

  // Build a dependency graph over the nodes to be erased.
  Graph g;

  for (Instruction *I : to_be_erased) {
    g.addNode(I);
  }

  for (auto &kv : versions) {
    Value *V = kv.first;
    const SmallPtrSet<Instruction *, 4> &set = kv.second;

    if (!g.hasNode(V))
      continue;

    for (Instruction *dep : set) {
      g.addEdge(V, dep);
    }
  }

  // Fix for bug #8: use a proper three-colour DFS (white/grey/black) so that
  // back-edges (cycles) are detected and skipped rather than causing infinite
  // recursion.  colour: 0 = white, 1 = grey (on stack), 2 = black (done).
  DenseMap<Value *, int> colour;
  for (Instruction *I : to_be_erased) {
    colour[I] = 0; // white
  }

  for (Instruction *I : to_be_erased) {
    if (colour[I] == 0) {
      visit(g, colour, topsort, I);
    }
  }

  return topsort;
}

void SSIfy::visit(Graph &g, DenseMap<Value *, int> &colour,
                  SmallVectorImpl<Instruction *> &list, Value *V) const {
  // Fix for bug #8: skip nodes that are already being processed (grey) to
  // break cycles, and skip nodes that are already finished (black).
  int &c = colour[V];
  if (c != 0) // grey (cycle) or black (already emitted)
    return;

  c = 1; // mark grey — currently on the DFS stack

  auto it = g.vertices.find(V);
  if (it != g.vertices.end()) {
    for (Value *m : it->second) {
      visit(g, colour, list, m);
    }
  }

  c = 2; // mark black — fully processed
  list.push_back(cast<Instruction>(V));
}
