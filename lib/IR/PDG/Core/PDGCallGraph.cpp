/**
 * @file PDGCallGraph.cpp
 * @brief Implementation of the call graph for the Program Dependency Graph
 *
 * This file implements a specialized call graph that captures the calling
 * relationships between functions in the program. The call graph is an
 * essential component of the PDG system that enables inter-procedural analysis
 * and dependency tracking.
 *
 * Key features:
 * - Construction of the call graph from LLVM Module
 * - Support for both direct and indirect function calls
 * - Integration with the overall PDG system
 * - Call site detection and management
 * - Support for call reachability analysis
 *
 * The call graph helps optimize PDG construction by providing information about
 * which functions can potentially call other functions, enabling more focused
 * dependency analysis.
 */

#include "IR/PDG/Core/PDGCallGraph.h"

using namespace llvm;

void pdg::PDGCallGraph::build(Module &M) {
  _built_module = &M;
  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    Node *n = new Node(F, GraphNodeType::FUNC);
    _val_node_map.insert(std::make_pair(&F, n));
    addNode(*n);
  }

  // connect nodes
  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    auto *caller_node = getNode(F);
    if (caller_node == nullptr)
      continue;

    for (auto inst_i = inst_begin(F); inst_i != inst_end(F); inst_i++) {
      try {
        if (CallBase *ci = dyn_cast<CallBase>(&*inst_i)) {
          auto *called_func = pdgutils::getCalledFunc(*ci);
          // direct calls
          if (called_func != nullptr) {
            auto *callee_node = getNode(*called_func);
            if (callee_node != nullptr)
              caller_node->addNeighbor(*callee_node,
                                       EdgeType::CONTROLDEP_CALLINV);
          } else {
            // indirect calls
            auto ind_call_candidates = getIndirectCallCandidates(*ci, M);
            for (auto *ind_call_can : ind_call_candidates) {
              Node *callee_node = getNode(*ind_call_can);
              if (callee_node != nullptr)
                caller_node->addNeighbor(*callee_node, EdgeType::IND_CALL);
            }
          }
        }
      } catch (...) {
        // Skip invalid call instructions
        errs() << "Warning: Skipping invalid call instruction in function "
               << F.getName() << "\n";
        continue;
      }
    }
  }

  _is_build = true;
}

bool pdg::PDGCallGraph::isFuncSignatureMatch(CallBase &ci, llvm::Function &f) {
  // don't handle varadic function at the moment
  if (f.isVarArg())
    return false;
  auto actual_arg_list_size = ci.arg_size();
  auto formal_arg_list_size = f.arg_size();
  if (actual_arg_list_size != formal_arg_list_size)
    return false;
  // compare return type
  auto *actual_ret_type = ci.getType();
  auto *formal_ret_type = f.getReturnType();
  if (!isTypeEqual(*actual_ret_type, *formal_ret_type))
    return false;

  for (unsigned i = 0; i < actual_arg_list_size; i++) {
    auto *actual_arg = ci.getArgOperand(i);
    auto *formal_arg = f.getArg(i);
    if (!isTypeEqual(*actual_arg->getType(), *formal_arg->getType()))
      return false;
  }

  return true;
}

bool pdg::PDGCallGraph::isTypeEqual(Type &t1, Type &t2) {
  // Fast path: same pointer (always true within one compilation unit).
  if (&t1 == &t2)
    return true;

  // Both must be the same kind of type.
  if (t1.getTypeID() != t2.getTypeID())
    return false;

  // For non-pointer types (int, float, void, …) the identity check above is
  // sufficient within a single module.  After llvm-link, primitive types are
  // still uniqued, so reaching here with non-pointer types means they differ.
  if (!t1.isPointerTy())
    return false;

  auto *t1_pointed_ty = t1.getPointerElementType();
  auto *t2_pointed_ty = t2.getPointerElementType();

  // Pointer-to-struct: compare struct names after stripping llvm-link version
  // tags (e.g. "struct.Foo.1" vs "struct.Foo").
  if (t1_pointed_ty->isStructTy() && t2_pointed_ty->isStructTy()) {
    auto t1_name =
        pdgutils::stripVersionTag(t1_pointed_ty->getStructName().str());
    auto t2_name =
        pdgutils::stripVersionTag(t2_pointed_ty->getStructName().str());
    return (t1_name == t2_name);
  }

  // Pointer-to-pointer: recurse.
  if (t1_pointed_ty->isPointerTy() && t2_pointed_ty->isPointerTy())
    return isTypeEqual(*t1_pointed_ty, *t2_pointed_ty);

  // Pointer-to-array: compare element type and size.
  if (t1_pointed_ty->isArrayTy() && t2_pointed_ty->isArrayTy()) {
    auto *at1 = cast<ArrayType>(t1_pointed_ty);
    auto *at2 = cast<ArrayType>(t2_pointed_ty);
    if (at1->getNumElements() != at2->getNumElements())
      return false;
    return isTypeEqual(*at1->getElementType(), *at2->getElementType());
  }

  // Pointer-to-function: compare via identity (function types are uniqued).
  if (t1_pointed_ty->isFunctionTy() && t2_pointed_ty->isFunctionTy())
    return (t1_pointed_ty == t2_pointed_ty);

  // Pointer-to-primitive (int, float, void, …): compare the pointee types.
  // After llvm-link, primitive types are still uniqued, so identity suffices.
  return (t1_pointed_ty == t2_pointed_ty);
}

std::set<Function *> pdg::PDGCallGraph::getIndirectCallCandidates(CallBase &ci,
                                                                  Module &M) {
  Type *call_func_ty = ci.getFunctionType();
  assert(call_func_ty != nullptr &&
         "cannot find indirect call for null function type!\n");
  std::set<Function *> ind_call_cand;
  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (isFuncSignatureMatch(ci, F))
      ind_call_cand.insert(&F);
  }
  return ind_call_cand;
}

bool pdg::PDGCallGraph::canReach(Node &src, Node &sink) {
  std::queue<Node *> node_queue;
  std::unordered_set<Node *> seen_node;
  node_queue.push(&src);
  while (!node_queue.empty()) {
    Node *n = node_queue.front();
    node_queue.pop();
    if (n == &sink)
      return true;
    if (seen_node.find(n) != seen_node.end())
      continue;
    seen_node.insert(n);

    for (auto *out_neighbor : n->getOutNeighbors()) {
      node_queue.push(out_neighbor);
    }
  }
  return false;
}

void pdg::PDGCallGraph::dump() {
  for (auto pair : _val_node_map) {
    if (Function *f = dyn_cast<Function>(pair.first)) {
      errs() << f->getName() << ": \n";
      for (auto *out_node : pair.second->getOutNeighbors()) {
        if (Function *callee = dyn_cast<Function>(out_node->getValue()))
          errs() << "\t\t" << callee->getName() << "\n";
      }
    }
  }
}

void pdg::PDGCallGraph::printPaths(Node &src, Node &sink) {
  auto pathes = computePaths(src, sink);
  unsigned count = 1;
  for (auto path : pathes) {
    errs() << "************* Printing Pathes **************\n";
    errs() << "path len: " << path.size() << "\n";
    for (auto iter = path.begin(); iter != path.end(); iter++) {
      errs() << (*iter)->getName();
      if (std::next(iter, 1) != path.end())
        errs() << " -> ";
      else
        errs() << "\n\b";
    }
    errs() << "********************************************\n";
    count++;
  }
}

pdg::PDGCallGraph::PathVecs pdg::PDGCallGraph::computePaths(Node &src,
                                                            Node &sink) {
  PathVecs ret;
  std::unordered_set<Function *> visited_funcs;
  bool found_path = false;
  computePathsHelper(ret, src, sink, {}, visited_funcs,
                     found_path); // just find one path
  return ret;
}

void pdg::PDGCallGraph::computePathsHelper(
    PathVecs &path_vecs, Node &src, Node &sink,
    std::vector<llvm::Function *> cur_path,
    std::unordered_set<llvm::Function *> visited_funcs, bool &found_path) {
  if (found_path)
    return;
  assert(isa<Function>(src.getValue()) &&
         "cannot process non function node (compute path, src)\n");
  assert(isa<Function>(sink.getValue()) &&
         "cannot process non function node (compute path, sink)\n");
  Function *src_func = cast<Function>(src.getValue());
  Function *sink_func = cast<Function>(sink.getValue());
  if (visited_funcs.find(src_func) != visited_funcs.end())
    return;
  visited_funcs.insert(src_func);
  cur_path.push_back(src_func);
  if (src_func == sink_func) {
    path_vecs.push_back(cur_path);
    found_path = true;
    return;
  }

  for (auto *out_neighbor : src.getOutNeighbors()) {
    computePathsHelper(path_vecs, *out_neighbor, sink, cur_path, visited_funcs,
                       found_path);
  }
}
