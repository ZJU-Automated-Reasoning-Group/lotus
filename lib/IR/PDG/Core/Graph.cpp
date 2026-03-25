/**
 * @file Graph.cpp
 * @brief Implementation of graph data structures for the Program Dependency
 * Graph
 *
 * This file implements the core graph data structures used in the PDG system:
 * - GenericGraph: A base graph class providing common graph operations
 * - ProgramGraph: A specific implementation for program dependency analysis
 *
 * Features include:
 * - Mapping LLVM Values to graph nodes
 * - Graph traversal and reachability analysis
 * - Node and edge creation and management
 * - Supporting field-sensitive analysis through tree structures
 * - Class hierarchy and function call relationship modeling
 *
 * The graph implementation allows for extensive queries about program
 * dependencies, supporting both control and data dependency analyses in an
 * integrated structure.
 */

#include "IR/PDG/Core/Graph.h"

using namespace llvm;

std::set<std::string> targetFuncNames = {
    "printInfo",
    "main",
    "changeName",
};

// Generic Graph
bool pdg::GenericGraph::hasNode(Value &v) {
  return (_val_node_map.find(&v) != _val_node_map.end());
}

pdg::Node *pdg::GenericGraph::getNode(Value &v) {
  if (!hasNode(v))
    return nullptr;
  return _val_node_map[&v];
}

// pretty print nodes and edges in PDG
void pdg::GenericGraph::dumpGraph() {
  // print nodes
  errs() << "=============== Node Set ===============\n";
  for (auto node_iter = begin(); node_iter != end(); ++node_iter) {
    auto *node = *node_iter;
    std::string str;
    raw_string_ostream OS(str);
    Value *val = node->getValue();
    if (val != nullptr) {
      if (Function *f = dyn_cast<Function>(val))
        OS << f->getName().str() << "\n";
      else
        OS << *val << "\n";
    }

    if (node->getValue() != nullptr)
      errs() << "node: " << node << " - " << pdgutils::rtrim(OS.str()) << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
    else
      errs() << "node: " << node << " - "
             << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
  }

  // print edges
  errs() << "=============== Edge Set ===============\n";
  for (auto node_iter = begin(); node_iter != end(); ++node_iter) {
    auto *node = *node_iter;
    for (auto *out_edge : node->getOutEdgeSet()) {
      errs() << "edge: " << out_edge << " / " << "src["
             << out_edge->getSrcNode() << "] / " << " dst["
             << out_edge->getDstNode() << "]" << " / "
             << pdgutils::getEdgeTypeStr(out_edge->getEdgeType()) << "\n";
    }
  }
}

// ===== Graph Traversal =====
// DFS search
/**
 * @brief Checks if a path exists between two nodes.
 *
 * @param src The source node.
 * @param dst The destination node.
 * @return true if reachable, false otherwise.
 */
bool pdg::GenericGraph::canReach(pdg::Node &src, pdg::Node &dst) {
  // TODO: prune by call graph rechability, improve traverse efficiency
  if (canReach(src, dst, {}))
    return true;
  return false;
}

/**
 * @brief Checks if a path exists between two nodes, excluding specific edge
 * types.
 *
 * Performs a DFS traversal avoiding edges of types listed in
 * exclude_edge_types.
 *
 * @param src The source node.
 * @param dst The destination node.
 * @param exclude_edge_types Set of edge types to ignore during traversal.
 * @return true if reachable, false otherwise.
 */
bool pdg::GenericGraph::canReach(pdg::Node &src, pdg::Node &dst,
                                 std::set<EdgeType> exclude_edge_types) {
  std::set<Node *> visited;
  std::stack<Node *> node_stack;
  node_stack.push(&src);

  while (!node_stack.empty()) {
    auto *current_node = node_stack.top();
    node_stack.pop();
    if (visited.find(current_node) != visited.end())
      continue;
    visited.insert(current_node);
    if (current_node == &dst)
      return true;
    for (auto *out_edge : current_node->getOutEdgeSet()) {
      // exclude path
      if (exclude_edge_types.find(out_edge->getEdgeType()) !=
          exclude_edge_types.end())
        continue;
      node_stack.push(out_edge->getDstNode());
    }
  }
  return false;
}

// PDG Specific
/**
 * @brief Builds the Program Graph from an LLVM Module.
 *
 * Iterates through the module to create nodes for:
 * - Global variables
 * - Functions and their instructions
 * - Formal parameter trees
 * - Call sites (creating CallWrappers)
 * - Annotations
 *
 * @param M The LLVM Module to process.
 */
void pdg::ProgramGraph::build(Module &M) {
  _built_module = &M;
  // build node for global variables
  for (auto &global_var : M.getGlobalList()) {
    auto *global_var_type = global_var.getType();
    // if (!global_var_type->isPointerTy() && !global_var_type->isStructTy())
    //   continue;
    DIType *global_var_di_type = dbgutils::getGlobalVarDIType(global_var);
    if (global_var_di_type == nullptr)
      continue;
    GraphNodeType node_type = GraphNodeType::VAR_STATICALLOCGLOBALSCOPE;
    if (pdgutils::isStaticFuncVar(global_var, M))
      node_type = GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
    else if (pdgutils::isStaticGlobalVar(global_var))
      node_type = GraphNodeType::VAR_STATICALLOCMODULESCOPE;

    Node *n = new Node(global_var, node_type);
    _val_node_map.insert(std::pair<Value *, Node *>(&global_var, n));
    addNode(*n);
  }

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    std::string func_name = F.getName().str();
    // bool isTargetFunc = false;
    // for (auto name : targetFuncNames)
    // {
    //   if (func_name.find(name) != std::string::npos)
    //   {
    //     isTargetFunc = true;
    //     break;
    //   }
    // }

    // if (!isTargetFunc)
    //   continue;

    // create nodes for inst in functions
    FunctionWrapper *func_w = new FunctionWrapper(&F);
    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      try {
        GraphNodeType node_type = GraphNodeType::INST_OTHER;
        if (isAnnotationCallInst(*inst_iter))
          node_type = GraphNodeType::ANNO_VAR;
        if (isa<ReturnInst>(&*inst_iter))
          node_type = GraphNodeType::INST_RET;
        if (isa<CallBase>(&*inst_iter))
          node_type = GraphNodeType::INST_FUNCALL;
        if (isa<BranchInst>(&*inst_iter))
          node_type = GraphNodeType::INST_BR;
        // if (auto invokeInst = dyn_cast<InvokeInst>(&*inst_iter))
        //   errs() << invokeInst->getCalledFunction()->getName() << "\n";
        Node *n = new Node(*inst_iter, node_type);
        _val_node_map.insert(std::pair<Value *, Node *>(&*inst_iter, n));
        func_w->addInst(*inst_iter);
        addNode(*n);
      } catch (...) {
        // Skip invalid instructions
        errs() << "Warning: Skipping invalid instruction in function "
               << F.getName() << "\n";
        continue;
      }
    }
    func_w->buildFormalTreeForArgs();
    func_w->buildFormalTreesForRetVal();
    addFormalTreeNodesToGraph(*func_w);
    addNode(*func_w->getEntryNode());
    _func_wrapper_map.insert(std::make_pair(&F, func_w));
    _val_node_map.insert(
        std::pair<Value *, Node *>(&F, func_w->getEntryNode()));
  }

  buildGlobalAnnotationNodes(M);

  // handle call sites
  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (!hasFuncWrapper(F))
      continue;

    FunctionWrapper *func_w = getFuncWrapper(F);
    auto call_insts = func_w->getCallInsts();
    for (auto *ci : call_insts) {
      auto *called_func = pdgutils::getCalledFunc(*ci);
      // indirect call, insert a call wrapper first, build parameter tree lazyly
      // when actually connectting indirect call site and called function during
      // PDG construction
      if (called_func == nullptr) {
        CallWrapper *cw = new CallWrapper(*ci);
        _call_wrapper_map.insert(std::make_pair(ci, cw));
        continue;
      }
      if (!hasFuncWrapper(*called_func))
        continue;
      CallWrapper *cw = new CallWrapper(*ci);
      FunctionWrapper *callee_fw = getFuncWrapper(*called_func);
      cw->buildActualTreeForArgs(*callee_fw);
      cw->buildActualTreesForRetVal(*callee_fw);
      cw->setHasParamTrees();
      _call_wrapper_map.insert(std::make_pair(ci, cw));
    }
  }
  _is_build = true;
}

void pdg::ProgramGraph::bindDITypeToNodes(Module &M) {
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    FunctionWrapper *fw = getFuncWrapper(F);
    if (!fw)
      continue;
    auto dbg_declare_insts = fw->getDbgDeclareInsts();
    // bind ditype to the top-level pointer (alloca)
    for (auto *dbg_declare_inst : dbg_declare_insts) {
      // For LLVM 14.0.0, we need to handle Metadata conversion correctly
      Metadata *MD = dbg_declare_inst->getRawLocation();
      if (!MD)
        continue;

      Value *addr = nullptr;
      if (auto *VMD = dyn_cast<ValueAsMetadata>(MD)) {
        addr = VMD->getValue();
      } else {
        continue;
      }

      Node *addr_node = getNode(*addr);
      if (!addr_node)
        continue;
      auto *DLV = dbg_declare_inst->getVariable(); // di local variable instance
      assert(DLV != nullptr &&
             "cannot find DILocalVariable Node for computing DIType");
      DIType *var_di_type = DLV->getType();
      std::string var_name = DLV->getName().str();
      assert(var_di_type != nullptr && "cannot bind nullptr ditype to node!");
      addr_node->setDIType(*var_di_type);
      _node_di_type_map.insert(std::make_pair(addr_node, var_di_type));
      // if dbg contains class information, create a separate class node
      if (dbgutils::isClassPointerType(*var_di_type)) {
        auto class_name = dbgutils::getSourceLevelTypeName(*var_di_type);
        if (_class_name_set.find(class_name) != _class_name_set.end()) {
          // if the variable name is this, the current function is a class
          // method
          if (var_name == "this" && fw->getClassName().empty())
            fw->setClassName(class_name);
          continue;
        }
        _class_name_set.insert(class_name);
        Node *class_node = new Node(GraphNodeType::CLASS);
        class_node->setDIType(*var_di_type);
        addNode(*class_node);
        _class_node_map.insert(std::make_pair(class_name, class_node));
      }
    }

    for (auto inst_iter = inst_begin(F); inst_iter != inst_end(F);
         inst_iter++) {
      Instruction &i = *inst_iter;
      Node *n = getNode(i);
      assert(n != nullptr && "cannot compute node di type for null node!\n");
      DIType *node_di_type = computeNodeDIType(*n);
      if (node_di_type != nullptr)
        n->setDIType(*node_di_type);
    }
  }

  for (auto &global_var : M.getGlobalList()) {
    Node *global_node = getNode(global_var);
    if (global_node != nullptr) {
      auto *dt = dbgutils::getGlobalVarDIType(global_var);
      if (dt != nullptr)
        global_node->setDIType(*dt);
    }
  }
}

/**
 * @brief Computes the DIType for a given node.
 *
 * Infers the DIType based on the node's underlying value (Alloca, Load, GEP,
 * Cast, etc.) and its relationships to other nodes (e.g., base address of a
 * GEP).
 *
 * @param n The node to compute DIType for.
 * @return The computed DIType, or nullptr if not determinable.
 */
DIType *pdg::ProgramGraph::computeNodeDIType(Node &n) {
  // local variable
  Function *func = n.getFunc();
  if (!func)
    return nullptr;
  Value *val = n.getValue();
  if (!val)
    return nullptr;

  // alloc inst
  if (isa<AllocaInst>(val))
    return n.getDIType();
  // load inst
  if (LoadInst *li = dyn_cast<LoadInst>(val)) {
    if (Instruction *load_addr =
            dyn_cast<Instruction>(li->getPointerOperand())) {
      Node *load_addr_node = getNode(*load_addr);
      if (!load_addr_node)
        return nullptr;
      DIType *load_addr_di_type = load_addr_node->getDIType();
      if (!load_addr_di_type)
        return nullptr;
      // DIType* retDIType = DIUtils::stripAttributes(sourceInstDIType);
      DIType *loaded_val_di_type = dbgutils::getBaseDIType(*load_addr_di_type);
      if (loaded_val_di_type != nullptr)
        return dbgutils::stripAttributes(*loaded_val_di_type);
      return loaded_val_di_type;
    }

    if (GlobalVariable *gv =
            dyn_cast<GlobalVariable>(li->getPointerOperand())) {
      DIType *global_var_di_type = dbgutils::getGlobalVarDIType(*gv);
      if (!global_var_di_type)
        return nullptr;
      return dbgutils::getBaseDIType(*global_var_di_type);
    }
  }
  // gep inst
  if (GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(val)) {
    Value *base_addr = gep->getPointerOperand();
    Node *base_addr_node = getNode(*base_addr);
    if (!base_addr_node)
      return nullptr;
    DIType *base_addr_di_type = base_addr_node->getDIType();
    if (!base_addr_di_type)
      return nullptr;
    DIType *base_addr_lowest_di_type =
        dbgutils::getLowestDIType(*base_addr_di_type);
    if (!base_addr_lowest_di_type)
      return nullptr;
    if (!dbgutils::isStructType(*base_addr_lowest_di_type))
      return nullptr;
    if (auto *dict = dyn_cast<DICompositeType>(base_addr_lowest_di_type)) {
      auto di_node_arr = dict->getElements();
      for (unsigned i = 0; i < di_node_arr.size(); ++i) {
        DIType *field_di_type = dyn_cast<DIType>(di_node_arr[i]);
        if (field_di_type == nullptr)
          continue;
        // assert(field_di_type != nullptr && "fail to retrive field di type
        // (computeNodeDIType)");
        if (pdgutils::isGEPOffsetMatchDIOffset(*field_di_type, *gep))
          return field_di_type;
      }
    }
  }
  // cast inst
  if (CastInst *cast_inst = dyn_cast<CastInst>(val)) {
    Value *casted_val = cast_inst->getOperand(0);
    Node *casted_val_node = getNode(*casted_val);
    if (!casted_val_node)
      return nullptr;
    return casted_val_node->getDIType();
  }

  // default
  return nullptr;
}

void pdg::ProgramGraph::addTreeNodesToGraph(pdg::Tree &tree) {
  TreeNode *root_node = tree.getRootNode();
  std::queue<TreeNode *> node_queue;
  node_queue.push(root_node);
  while (!node_queue.empty()) {
    TreeNode *current_node = node_queue.front();
    node_queue.pop();
    addNode(*current_node);
    for (auto *child_node : current_node->getChildNodes()) {
      node_queue.push(child_node);
    }
  }
}

void pdg::ProgramGraph::addFormalTreeNodesToGraph(FunctionWrapper &func_w) {
  for (auto *arg : func_w.getArgList()) {
    Tree *formal_in_tree = func_w.getArgFormalInTree(*arg);
    Tree *formal_out_tree = func_w.getArgFormalOutTree(*arg);
    if (formal_in_tree)
      addTreeNodesToGraph(*formal_in_tree);
    if (formal_out_tree)
      addTreeNodesToGraph(*formal_out_tree);
  }
}

bool pdg::ProgramGraph::isAnnotationCallInst(Instruction &inst) {
  if (CallBase *ci = dyn_cast<CallBase>(&inst)) {
    Function *f = pdgutils::getCalledFunc(*ci);
    if (f == nullptr)
      return false;
    std::string called_func_name = f->getName().str();
    if (called_func_name == "llvm.var.annotation")
      return true;
  }
  return false;
}

void pdg::ProgramGraph::buildGlobalAnnotationNodes(Module &M) {
  auto *global_annos = M.getNamedGlobal("llvm.global.annotations");
  if (global_annos) {
    // build a node for the annotation
    Node *global_anno_node =
        new Node(*global_annos, GraphNodeType::ANNO_GLOBAL);
    _val_node_map.insert(
        std::pair<Value *, Node *>(global_annos, global_anno_node));
    addNode(*global_anno_node);
    auto *casted_array = cast<ConstantArray>(global_annos->getOperand(0));
    for (unsigned i = 0; i < casted_array->getNumOperands(); i++) {
      auto *casted_struct = cast<ConstantStruct>(casted_array->getOperand(i));
      if (auto *annotated_gv = dyn_cast<GlobalValue>(
              casted_struct->getOperand(0)->getOperand(0))) {
        auto *globalSenStr =
            cast<GlobalVariable>(casted_struct->getOperand(1)->getOperand(0));
        auto anno = cast<ConstantDataArray>(globalSenStr->getOperand(0))
                        ->getAsCString();
        Node *n = getNode(*annotated_gv);
        if (n == nullptr) {
          n = new Node(*annotated_gv,
                       GraphNodeType::VAR_STATICALLOCGLOBALSCOPE);
          _val_node_map.insert(std::pair<Value *, Node *>(annotated_gv, n));
          addNode(*n);
        }
        n->addNeighbor(*global_anno_node, EdgeType::ANNO_GLOBAL);
      }
    }
  }
}

pdg::Node *pdg::ProgramGraph::getClassNodeByName(std::string cls_name) {
  auto it = _class_node_map.find(cls_name);
  if (it == _class_node_map.end())
    return nullptr;
  return _class_node_map[cls_name];
}
