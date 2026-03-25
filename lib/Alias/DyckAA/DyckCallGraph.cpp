/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Alias/DyckAA/DyckCallGraph.h"

static cl::opt<bool> WithEdgeLabels(
    "with-labels", cl::init(false), cl::Hidden,
    cl::desc("Determine whether there are edge lables in the cg."));

DyckCallGraph::DyckCallGraph()
    : ExternalCallingNode(getOrInsertFunction(nullptr)) {}

DyckCallGraph::~DyckCallGraph() {
  auto It = FunctionMap.begin();
  while (It != FunctionMap.end()) {
    delete (It->second);
    It++;
  }
  FunctionMapTy().swap(FunctionMap);
}

DyckCallGraphNode *DyckCallGraph::getOrInsertFunction(Function *Func) {
  auto It = FunctionMap.find(Func);
  if (It == FunctionMap.end()) {
    auto *Ret = new DyckCallGraphNode(Func);

    // The following if-statement is copied from llvm's call graph
    // implementation If this function has external linkage or has its address
    // taken and it is not a callback, then anything could call it.
    if (Func && (!Func->hasLocalLinkage() ||
                 Func->hasAddressTaken(nullptr, /*IgnoreCallbackUses=*/true)))
      ExternalCallingNode->addCalledFunction(nullptr, Ret);

    FunctionMap.emplace(Func, Ret);
    return Ret;
  }
  return It->second;
}

DyckCallGraphNode *DyckCallGraph::getFunction(Function *Func) const {
  auto It = FunctionMap.find(Func);
  if (It == FunctionMap.end())
    return nullptr;
  return It->second;
}

void DyckCallGraph::dotCallGraph(const std::string &ModuleIdentifier) {
  std::string DotFileName;
  DotFileName.append(ModuleIdentifier);
  DotFileName.append(".maycg.dot");

  FILE *FOut = fopen(DotFileName.data(), "w+");
  fprintf(FOut, "digraph maycg {\n");

  auto FWIt = FunctionMap.begin();
  while (FWIt != FunctionMap.end()) {
    DyckCallGraphNode *FW = FWIt->second;
    if (FWIt->first) {
      fprintf(FOut, "\tf%p[label=\"%s\"]\n", FW,
              FW->getLLVMFunction()->getName().data());
    }
    FWIt++;
  }

  FWIt = FunctionMap.begin();
  while (FWIt != FunctionMap.end()) {
    DyckCallGraphNode *FW = FWIt->second;
    auto CCIt = FW->common_call_begin();
    while (CCIt != FW->common_call_end()) {
      CommonCall *CC = *CCIt;
      auto *Callee = CC->getCalledFunction();

      if (FunctionMap.count(Callee)) {
        if (WithEdgeLabels) {
          Value *CI = CC->getInstruction();
          std::string S;
          raw_string_ostream RSO(S);
          if (CI != nullptr) {
            RSO << *(CI);
          } else {
            RSO << "Hidden";
          }
          std::string &EdgeLabelStr = RSO.str();
          for (char &C : EdgeLabelStr) {
            if (C == '\"') {
              C = '`';
            }

            if (C == '\n') {
              C = ' ';
            }
          }
          fprintf(FOut, "\tf%p->f%p[label=\"%s\"]\n", FW, FunctionMap[Callee],
                  EdgeLabelStr.data());
        } else {
          fprintf(FOut, "\tf%p->f%p\n", FW, FunctionMap[Callee]);
        }
      } else {
        llvm_unreachable("ERROR in printCG when print common function calls.");
      }
      CCIt++;
    }

    auto FPIt = FW->pointer_call_begin();
    while (FPIt != FW->pointer_call_end()) {
      PointerCall *PC = *FPIt;
      char *EdgeLabelData = nullptr;
      if (WithEdgeLabels) {
        std::string S;
        raw_string_ostream RSO(S);
        if (PC->getInstruction()) {
          RSO << *(PC->getInstruction());
        } else {
          RSO << "Hidden";
        }
        std::string &EdgeLabelStr = RSO.str(); // edge label is the call inst
        for (char &C : EdgeLabelStr) {
          if (C == '\"') {
            C = '`';
          }

          if (C == '\n') {
            C = ' ';
          }
        }
        EdgeLabelData = const_cast<char *>(EdgeLabelStr.data());
      }
      auto MCIt = PC->begin();
      while (MCIt != PC->end()) {
        Function *MCF = *MCIt;
        if (FunctionMap.count(MCF)) {
          if (WithEdgeLabels) {
            fprintf(FOut, "\tf%p->f%p[label=\"%s\"]\n", FW, FunctionMap[MCF],
                    EdgeLabelData);
          } else {
            fprintf(FOut, "\tf%p->f%p\n", FW, FunctionMap[MCF]);
          }
        } else {
          llvm_unreachable("ERROR in printCG when print fp calls.");
        }
        MCIt++;
      }
      FPIt++;
    }
    FWIt++;
  }

  fprintf(FOut, "}\n");
  fclose(FOut);
}

void DyckCallGraph::printFunctionPointersInformation(
    const std::string &ModuleIdentifier) {
  std::string Dotfilename;
  Dotfilename.append(ModuleIdentifier);
  Dotfilename.append(".fp.txt");

  FILE *FOut = fopen(Dotfilename.data(), "w+");

  auto FWIt = this->begin();
  while (FWIt != this->end()) {
    DyckCallGraphNode *FW = FWIt->second;

    auto FPIt = FW->pointer_call_begin();
    while (FPIt != FW->pointer_call_end()) {
      /*Value * callInst = fpIt->first;
      std::string s;
      raw_string_ostream rso(s);
      rso << *(callInst);
      string& edgelabel = rso.str();
      for (unsigned int i = 0; i < edgelabel.length(); i++) {
          if (edgelabel[i] == '\"') {
              edgelabel[i] = '`';
          }

          if (edgelabel[i] == '\n') {
              edgelabel[i] = ' ';
          }
      }
      fprintf(fout, "CallInst: %s\n", edgelabel.data()); //call inst
       */
      auto *PC = *FPIt;
      fprintf(FOut, "%d\n", PC->size()); // number of functions

      // what functions?
      auto MCIt = PC->begin();
      while (MCIt != PC->end()) {
        // Function * mcf = *mcIt;
        // fprintf(fout, "%s\n", mcf->getName().data());

        MCIt++;
      }

      // fprintf(fout, "\n");

      FPIt++;
    }

    FWIt++;
  }

  fclose(FOut);
}
