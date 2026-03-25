#include "Solvers/SMT/QuantSimp/EGraphsSimp.h"

#include <algorithm>

namespace EGraphs {

// QuantifierArgs implementation
QuantifierArgs::QuantifierArgs(Z3_ast ast, z3::context *ctx) {
  unsigned numBound = Z3_get_quantifier_num_bound(*ctx, ast);

  Z3_sort *sorts = (Z3_sort *)malloc(sizeof(Z3_sort) * numBound);
  Z3_symbol *decl_names = (Z3_symbol *)malloc(sizeof(Z3_symbol) * numBound);
  for (unsigned i = 0; i < numBound; i++) {
    sorts[i] = Z3_get_quantifier_bound_sort(*ctx, ast, i);
    decl_names[i] = Z3_get_quantifier_bound_name(*ctx, ast, i);
  }

  unsigned numPatterns = Z3_get_quantifier_num_patterns(*ctx, ast);
  Z3_pattern *patterns = (Z3_pattern *)malloc(sizeof(Z3_pattern) * numPatterns);
  for (unsigned i = 0; i < numPatterns; i++) {
    patterns[i] = Z3_get_quantifier_pattern_ast(*ctx, ast, i);
  }

  this->is_forall = Z3_is_quantifier_forall(*ctx, ast);
  this->weight = Z3_get_quantifier_weight(*ctx, ast);
  this->num_patterns = numPatterns;
  this->num_decls = numBound;
  this->sorts = sorts;
  this->decl_names = decl_names;
  this->patterns = patterns;
}

QuantifierArgs::~QuantifierArgs() {
  free(this->sorts);
  free(this->decl_names);
  free(this->patterns);
}

// Function implementation
Function::Function(std::vector<Function *> *inputs, z3::expr value) {
  this->IsQuantifier = false;
  this->IsBoundVar = false;
  this->UsedBy = new std::vector<Function *>();
  this->Parent = this;
  this->Inputs = inputs;
  this->Value = Z3_func_decl(value.decl());
  for (Function *func : *inputs) {
    func->UsedBy->push_back(this);
  }
}

Function::Function(QuantifierArgs *args, Function *body) {
  this->IsQuantifier = true;
  this->IsBoundVar = false;
  this->UsedBy = new std::vector<Function *>();
  this->Parent = this;
  this->Inputs = new std::vector<Function *>{body};
  this->Value = (Z3_func_decl)0;
  this->quantifierArgs = args;
  body->UsedBy->push_back(this);
}

Function::Function(Z3_ast boundVar) {
  this->IsBoundVar = true;
  this->IsQuantifier = false;
  this->UsedBy = new std::vector<Function *>();
  this->Parent = this;
  this->Inputs = new std::vector<Function *>();
  this->Value = (Z3_func_decl)0;
  this->boundVar = boundVar;
}

void Function::ManualDestroy() {
  if (this->UsedBy->empty()) {
    for (Function *func : *this->Inputs) {
      func->UsedBy->erase(
          std::remove(func->UsedBy->begin(), func->UsedBy->end(), this),
          func->UsedBy->end());
    }
    delete this->UsedBy;
    delete this->Inputs;
    if (IsQuantifier)
      delete this->quantifierArgs;
    delete this;
  }
}

Z3_func_decl Function::getName() const { return this->Value; }

Function *Function::GetRoot() {
  if (this->Parent == this) {
    return this;
  }
  Function *root = this->Parent->GetRoot();
  this->Parent = root;
  return root;
}

// tests complete equality, as in exact same node
bool Function::operator==(const Function &other) const {
  if (this->IsBoundVar != other.IsBoundVar)
    return false;

  if (this->IsBoundVar && (this->boundVar != other.boundVar))
    return false;

  if (this->IsQuantifier != other.IsQuantifier)
    return false;

  if (this->IsQuantifier &&
      (this->quantifierArgs->is_forall != other.quantifierArgs->is_forall ||
       this->quantifierArgs->weight != other.quantifierArgs->weight ||
       this->quantifierArgs->num_patterns !=
           other.quantifierArgs->num_patterns ||
       this->quantifierArgs->num_decls != other.quantifierArgs->num_decls))
    return false;

  if (this->Inputs->size() != other.Inputs->size() ||
      this->getName() != other.getName()) {
    return false;
  }
  for (size_t i = 0; i < this->Inputs->size(); i++) {
    if (!(*(*(this->Inputs))[i] == *(*(other.Inputs))[i])) {
      return false;
    }
  }
  return true;
}

bool Function::operator!=(const Function &other) const {
  return !(*this == other);
}

bool Function::IsEquivalent(Function *other) {
  if (this->GetRoot() == other->GetRoot()) {
    return true;
  }
  return this->IsCongruent(other);
}

bool Function::IsCongruent(Function *other) {
  if (this->getName() != other->getName()) {
    return false;
  }
  if (this->Inputs->size() != other->Inputs->size()) {
    return false;
  }
  for (size_t i = 0; i < this->Inputs->size(); i++) {
    if ((*this->Inputs)[i]->GetRoot() != (*other->Inputs)[i]->GetRoot()) {
      return false;
    }
  }
  return true;
}

// used to not create multiple nodes for the same term/formula
bool TryGetRealFunction(
    Function *function,
    std::map<Z3_func_decl, std::vector<Function *> *> functions,
    Function **outFunction) {
  if (functions.find(function->getName()) == functions.end()) {
    return false;
  }
  for (Function *realFunction : *functions[function->getName()]) {
    if (*realFunction == *function) {
      *outFunction = realFunction;
      return true;
    }
  }
  return false;
}

// EGraph implementation
EGraph::EGraph(z3::context *ctx) {
  this->_quantified_variables = std::set<Function *>();
  this->_functions = std::map<Z3_func_decl, std::vector<Function *> *>{};
  this->_in_equalities = std::vector<Function *>();
  this->_class = std::map<Function *, std::vector<Function *> *>();
  this->ctx = ctx;
}

EGraph::~EGraph() {
  for (auto it = this->_functions.begin(); it != this->_functions.end(); it++) {
    for (Function *f : *it->second) {
      delete f->Inputs;
      delete f->UsedBy;
      delete f;
    }
    delete it->second;
  }
  for (auto it = this->_class.begin(); it != this->_class.end(); it++) {
    delete it->second;
  }
}

// functions for parsing/transforming z3::expr to EGraph
EGraph *EGraph::ExprToEGraph(z3::expr expr, z3::context *ctx) {
  EGraph *graph = new EGraph(ctx);
  graph->ParseAnd(expr);

  return graph;
}

void EGraph::ParseAnd(z3::expr expr) {
  int numArgs = expr.num_args();
  for (int i = 0; i < numArgs; i++) {
    if (expr.arg(i).is_and()) {
      this->ParseAnd(expr.arg(i));
      continue;
    }
    if (expr.arg(i).is_eq()) {
      this->ParseEq(expr.arg(i));
      continue;
    }
    this->ParsePredicate(expr.arg(i));
  }
}

void EGraph::ParseEq(z3::expr expr) {
  std::vector<Function *> vec = std::vector<Function *>();
  int numArgs = expr.num_args();
  for (int i = 0; i < numArgs; i++) {
    vec.push_back(this->ParseOther(expr.arg(i)));
  }
  this->AddEquality(vec[0], vec[1], expr);
}

void EGraph::ParsePredicate(z3::expr expr) {
  if (expr.is_app()) {
    std::vector<Function *> *arguments = new std::vector<Function *>();
    unsigned int numArgs = expr.num_args();
    for (unsigned i = 0; i < numArgs; i++) {
      arguments->push_back(ParseOther(expr.arg(i)));
    }
    AddPredicate(arguments, expr);
    return;
  }
  if (expr.is_quantifier()) {
    Function *quantifier = ParseOther(expr);
    _in_equalities.push_back(quantifier);
    return;
  }
  // to the best of my knowledge, this shouldn't happen
  return;
}

// this is eventually called by the above functions and it returns the created
// node for this reason
Function *EGraph::ParseOther(z3::expr expr) {
  if (!expr.is_const() && expr.is_app()) {
    // it's a function with >0 arguments
    std::vector<Function *> *arguments = new std::vector<Function *>();
    unsigned numArgs = expr.num_args();
    for (unsigned i = 0; i < numArgs; i++) {
      arguments->push_back(ParseOther(expr.arg(i)));
    }
    return AddFunction(arguments, expr);
  }
  if (expr.is_quantifier()) {
    Z3_ast ast = (Z3_ast)expr;
    QuantifierArgs *quantifierArgs = new QuantifierArgs(ast, this->ctx);
    return AddQuantifier(quantifierArgs, ParseOther(expr.body()));
  }
  if (expr.is_var() && !expr.is_app()) {
    return AddBoundVar(expr);
  }
  // .to_string() gives exact number in case of number, otherwise it gives the
  // declaration, but declaration for number is always "bv"
  if (expr.to_string() == expr.decl().name().str()) {
    return AddQuantifiedVariable(expr);
  }
  // it's a number, so not quantified
  return AddTerm(expr);
}

// functions for building z3::expr
z3::expr EGraph::ToFormula(std::map<Function *, Function *> *repr,
                           std::set<Function *> *core) {
  z3::expr_vector arguments(*this->ctx);
  for (Function *in_equality : _in_equalities) {
    arguments.push_back(NodeToTerm(in_equality, repr));
  }
  for (auto elem : *repr) {
    z3::expr term = NodeToTerm(elem.second, repr);
    for (Function *InSameClass : *_class[elem.second->GetRoot()]) {
      if (InSameClass == elem.second || core->count(InSameClass) == 0) {
        continue;
      }
      arguments.push_back(term == NodeToTerm(InSameClass, repr));
    }
  }
  return z3::mk_and(arguments);
}

z3::expr EGraph::NodeToTerm(Function *node,
                            std::map<Function *, Function *> *repr) {
  if (node->IsBoundVar)
    return z3::expr(*this->ctx, node->boundVar);
  if (node->IsQuantifier) {
    QuantifierArgs *quantifierArgs = node->quantifierArgs;
    Z3_ast ast = Z3_mk_quantifier(
        *this->ctx, quantifierArgs->is_forall, quantifierArgs->weight,
        quantifierArgs->num_patterns, quantifierArgs->patterns,
        quantifierArgs->num_decls, quantifierArgs->sorts,
        quantifierArgs->decl_names, NodeToTerm((*node->Inputs)[0], repr));
    return z3::expr(*this->ctx, ast);
  }
  if (node->Inputs->size() == 0) {
    return z3::func_decl(*this->ctx, node->getName())();
  }
  z3::expr_vector arguments(*this->ctx);
  for (Function *arg : *node->Inputs) {
    arguments.push_back(NodeToTerm((*repr)[arg], repr));
  }
  return (z3::func_decl(*this->ctx, node->getName()))(arguments);
}

// add components
Function *EGraph::AddQuantifiedVariable(z3::expr value) {
  Function *term = AddTerm(value);
  _quantified_variables.insert(term);
  return term;
}

Function *EGraph::AddTerm(z3::expr value) {
  Z3_func_decl name = Z3_func_decl(value.decl());
  if (this->_functions.find(name) == this->_functions.end()) {
    Function *term = new Function(new std::vector<Function *>{}, value);
    this->_functions[name] =
        new std::vector<Function *>{term}; // if term didn't exist, make it
    this->_class[this->_functions[name]->at(0)] =
        new std::vector<Function *>{term};
  }
  return this->_functions[name]->at(0);
}

Function *EGraph::AddQuantifier(QuantifierArgs *args, Function *body) {
  Z3_func_decl name = (Z3_func_decl)0;
  if (this->_functions.find(name) == this->_functions.end()) {
    Function *func = new Function(args, body);
    this->_functions[name] =
        new std::vector<Function *>{func}; // if function didn't exist, make it
    this->_class[this->_functions[name]->at(0)] =
        new std::vector<Function *>{func};
    return this->_functions[name]->at(0);
  }
  Function *temporary = new Function(args, body);
  for (Function *func : *this->_functions[name]) {
    if (*temporary == *func) {
      temporary->ManualDestroy();
      return func;
    }
  }
  this->_functions[name]->push_back(temporary);
  this->_class[this->_functions[name]->back()] =
      new std::vector<Function *>{temporary};
  return temporary;
}

Function *EGraph::AddBoundVar(z3::expr expr) {
  Z3_func_decl name = (Z3_func_decl)0;
  if (this->_functions.find(name) == this->_functions.end()) {
    Function *func = new Function((Z3_ast)expr);
    this->_functions[name] =
        new std::vector<Function *>{func}; // if function didn't exist, make it
    this->_class[this->_functions[name]->at(0)] =
        new std::vector<Function *>{func};
    return this->_functions[name]->at(0);
  }
  Function *temporary = new Function((Z3_ast)expr);
  for (Function *func : *this->_functions[name]) {
    if (*temporary == *func) {
      temporary->ManualDestroy();
      return func;
    }
  }
  this->_functions[name]->push_back(temporary);
  this->_class[this->_functions[name]->back()] =
      new std::vector<Function *>{temporary};
  return temporary;
}

void EGraph::AddPredicate(std::vector<Function *> *functions, z3::expr value) {
  for (size_t i = 0; i < functions->size(); i++) {
    Function *oldFunction = (*functions)[i];
    if (TryGetRealFunction(oldFunction, this->_functions, &(*functions)[i])) {
      // at some point this used to cause an error and I don't know why, but
      // not calling it doesn't seem to have much of an impact
      // oldFunction->ManualDestroy();
    }
  }
  Function *newFunc = this->AddFunction(functions, value);
  this->_in_equalities.push_back(newFunc);
}

void EGraph::AddEquality(Function *first, Function *second, z3::expr value) {
  Function *realFirst = first;
  if (TryGetRealFunction(first, this->_functions, &realFirst)) {
    if (first != realFirst) {
      first->ManualDestroy();
    }
  }

  Function *realSecond = second;
  if (TryGetRealFunction(second, this->_functions, &realSecond)) {
    if (second != realSecond) {
      second->ManualDestroy();
    }
  }

  MakeEqual(realFirst, realSecond);
  for (Function *func : *realFirst->UsedBy) {
    CheckEqualities(func);
  }
}

Function *EGraph::AddFunction(std::vector<Function *> *inputs, z3::expr value) {
  Z3_func_decl name = Z3_func_decl(value.decl());
  if (this->_functions.find(name) == this->_functions.end()) {
    Function *func = new Function(inputs, value);
    this->_functions[name] =
        new std::vector<Function *>{func}; // if function didn't exist, make it
    this->_class[this->_functions[name]->at(0)] =
        new std::vector<Function *>{func};
    return this->_functions[name]->at(0);
  }

  Function *temporary = new Function(inputs, value);
  for (Function *func : *this->_functions[name]) {
    if (*temporary == *func) {
      temporary->ManualDestroy();
      return func;
    }
  }
  this->_functions[name]->push_back(temporary);
  this->_class[this->_functions[name]->back()] =
      new std::vector<Function *>{temporary};
  CheckEqualities(temporary);
  return temporary;
}

// functions for equalities
void EGraph::MakeEqual(Function *first, Function *second) {
  if (first->GetRoot() == second->GetRoot()) {
    return;
  }
  Function *root = second->GetRoot();
  root->Parent = first->GetRoot();
  // the recursive searching turned out to be a very expensive operation and
  // leaving it out increases the speed of the tests by around 6% to leave it
  // out, comment out lines 546 and 551-557
  unsigned int size = this->_class[first->GetRoot()]->size();
  // concat
  this->_class[first->GetRoot()]->insert(this->_class[first->GetRoot()]->end(),
                                         this->_class[root]->begin(),
                                         this->_class[root]->end());
  delete this->_class[root];
  this->_class.erase(root);
  for (int i = 0; i < size; i++) {
    for (Function *func : *(*this->_class[first->GetRoot()])[0]->UsedBy) {
      CheckEqualities(func);
    }
  }
}

void EGraph::CheckEqualities(Function *func) {
  for (size_t i = 0; i < this->_functions[func->getName()]->size() - 1; i++) {
    if (func->IsEquivalent((*this->_functions[func->getName()])[i])) {
      MakeEqual(func, (*this->_functions[func->getName()])[i]);
    }
  }
}

// QEL functions
std::set<Function *> *EGraph::FindCore(std::map<Function *, Function *> *repr) {
  std::set<Function *> *core = new std::set<Function *>();
  for (auto elem : *repr) {
    // every representative is in core
    core->insert(elem.second);
    for (Function *func : *_class[elem.second->GetRoot()]) {
      if (func == elem.second || _quantified_variables.count(func) != 0) {
        continue;
      }

      bool congruentFound = false;
      for (Function *InCore : *core) {
        if (InCore->IsCongruent(func)) {
          congruentFound = true;
          break;
        }
      }
      // if there is no congrunt function application in core, add it
      if (!congruentFound) {
        core->insert(func);
      }
    }
  }
  return core;
}

bool EGraph::IsGround(Function *function) {
  // this happens when the function is a Constant
  if (function->Inputs->size() == 0) {
    for (Function *variable : _quantified_variables) {
      // when function is found in the list of quantified variables, return
      // false
      if (*variable == *function) {
        return false;
      }
    }
    return true;
  }
  // when it is not a Constant, check all its inputs
  for (Function *input : *function->Inputs) {
    // if at least one is not ground, return false
    if (!IsGround(input)) {
      return false;
    }
  }
  return true;
}

std::map<Function *, Function *> *EGraph::FindDefs() {
  // Initialize representative function
  std::map<Function *, Function *> *repr =
      new std::map<Function *, Function *>();
  std::vector<Function *> groundTerms = std::vector<Function *>();

  // process every ground term
  for (auto pair = _functions.begin(); pair != _functions.end(); pair++) {
    for (Function *function : *pair->second) {
      if (!IsGround(function))
        continue;
      groundTerms.push_back(function);
    }
  }
  repr = AssignRepresentatives(repr, groundTerms);

  // process leftover terms
  std::vector<Function *> leftoverTerms = std::vector<Function *>();
  for (auto pair = _functions.begin(); pair != _functions.end(); pair++) {
    for (Function *function : *pair->second) {
      leftoverTerms.push_back(function);
    }
  }
  repr = AssignRepresentatives(repr, leftoverTerms);

  return repr;
}

std::map<Function *, Function *> *
EGraph::AssignRepresentatives(std::map<Function *, Function *> *repr,
                              std::vector<Function *> toBeAssigned) {
  // iterate through functions and terms while possible
  for (size_t i = 0; i < toBeAssigned.size(); i++) {
    Function *function = toBeAssigned[i];
    // if they have a representative, skip
    if (repr->find(function) != repr->end()) {
      continue;
    }
    for (Function *func : *_class[function->GetRoot()]) {
      // set the representative of everything equivalent to self, to self
      (*repr)[func] = function;
      for (Function *parent : *func->UsedBy) {
        bool isGround = true;
        // check if parent became ground term
        for (Function *child : *parent->Inputs) {
          if (repr->find(child) == repr->end()) {
            isGround = false;
            break;
          }
        }
        // assign it later if yes
        if (isGround) {
          toBeAssigned.push_back(parent);
        }
      }
    }
  }
  return repr;
}

bool EGraph::MakesCycle(Function *NewGround,
                        std::map<Function *, Function *> *repr) {
  std::map<Function *, bool> *ColoredGraph = new std::map<Function *, bool>();

  // initialize false as the value for every node of EGraph at the start
  std::map<Z3_func_decl, std::vector<Function *> *>::iterator it;
  for (it = this->_functions.begin(); it != this->_functions.end(); it++) {
    for (Function *func : *it->second) {
      (*ColoredGraph)[func] = false;
    }
  }

  for (Function *descendant : *NewGround->Inputs) {
    if (MakesCycleAux(NewGround, repr, descendant, ColoredGraph)) {
      delete ColoredGraph;
      return true;
    }
  }
  delete ColoredGraph;

  return false;
}

bool EGraph::MakesCycleAux(Function *NewGround,
                           std::map<Function *, Function *> *repr,
                           Function *current,
                           std::map<Function *, bool> *ColoredGraph) {
  // found the starting point
  if (current->GetRoot() == NewGround->GetRoot()) {
    return true;
  }
  current = (*repr)[current];
  for (Function *descendant : *current->Inputs) {
    if ((*ColoredGraph)[current]) {
      continue;
    }

    if (MakesCycleAux(NewGround, repr, descendant, ColoredGraph)) {
      return true;
    }
    (*ColoredGraph)[current] = true;
  }
  return false;
}

std::map<Function *, Function *> *
EGraph::RefineDefs(std::map<Function *, Function *> *repr) {
  for (Function *function : this->_quantified_variables) {
    if ((*repr)[function] != function) {
      continue; // quantified variable already represented by something else
    }
    Function *NewGround = function;

    for (Function *InSameClass : *_class[function->GetRoot()]) {
      if (InSameClass == function ||
          this->_quantified_variables.count(InSameClass) != 0 ||
          this->MakesCycle(InSameClass, repr)) {
        continue; // quantified variable shouldn't be represented by self,
                  // another quantified variable and it shouldn't create a
                  // cycle
      }
      NewGround = InSameClass;
      break;
    }

    for (Function *InSameClass : *_class[function->GetRoot()]) {
      (*repr)[InSameClass] = NewGround;
    }
  }
  return repr;
}

// this is QEL
z3::expr EGraph::Simplify(z3::expr expr, z3::context *ctx) {
  if (!expr.is_and()) {
    return expr;
  }
  EGraph *graph = ExprToEGraph(expr, ctx);
  auto *repr = graph->FindDefs();
  repr = graph->RefineDefs(repr);
  auto *core = graph->FindCore(repr);
  z3::expr res = graph->ToFormula(repr, core);

  delete graph;
  delete repr;
  delete core;
  return res;
}

} // namespace EGraphs
