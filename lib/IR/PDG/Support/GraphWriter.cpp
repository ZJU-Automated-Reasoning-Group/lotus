/**
 * @file GraphWriter.cpp
 * @brief Implementation of graph visualization utilities for the PDG system
 *
 * This file provides functionality for visualizing the Program Dependency Graph
 * in formats like DOT (Graphviz). It allows developers to generate visual
 * representations of the PDG for debugging and analysis purposes.
 *
 * Key functionality:
 * - Generation of DOT format graph representations
 * - Support for different graph visualization styles and formats
 * - Node and edge formatting for better visual distinction
 * - Integration with LLVM's graph writing infrastructure
 *
 * The visualization capabilities are particularly helpful for understanding
 * complex dependency relationships in programs and for debugging the PDG
 * construction process itself.
 */

#include "IR/PDG/Support/GraphWriter.h"

char pdg::ProgramDependencyPrinter::ID = 0;
using namespace llvm;

bool pdg::DOTONLYDDG;
bool pdg::DOTONLYCDG;

cl::opt<bool, true> DOTDDG("dot-only-ddg",
                           cl::desc("Only print ddg dependencies"),
                           cl::value_desc("dot print ddg deps"),
                           cl::location(pdg::DOTONLYDDG), cl::init(false));

cl::opt<bool, true> DOTCDG("dot-only-cdg",
                           cl::desc("Only print cdg dependencies"),
                           cl::value_desc("dot print cdg deps"),
                           cl::location(pdg::DOTONLYCDG), cl::init(false));

static RegisterPass<pdg::ProgramDependencyPrinter>
    PDGPrinter("dot-pdg",
               "Print instruction-level program dependency graph of "
               "function to 'dot' file",
               false, false);