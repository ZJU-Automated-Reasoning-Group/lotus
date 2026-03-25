#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

class ICFG;

std::string EscapeString(const std::string &Label) {
  std::string Str(Label);
  for (unsigned i = 0; i != Str.length(); ++i)
    switch (Str[i]) {
    case '\n':
      Str.insert(Str.begin() + i, '\\'); // Escape character...
      ++i;
      Str[i] = 'n';
      break;
    case '\t':
      Str.insert(Str.begin() + i, ' '); // Convert to two spaces
      ++i;
      Str[i] = ' ';
      break;
    case '\\':
      if (i + 1 != Str.length())
        switch (Str[i + 1]) {
        case 'l':
          continue; // don't disturb \l
        case '|':
        case '{':
        case '}':
          Str.erase(Str.begin() + i);
          continue;
        default:
          break;
        }
      // LLVM_FALLTHROUGH;
    case '{':
    case '}':
    case '<':
    case '>':
    case '|':
    case '"':
      Str.insert(Str.begin() + i, '\\'); // Escape character...
      ++i;                               // don't infinite loop
      break;
    }
  return Str;
}

class FunctionGraphWriter {

  llvm::raw_ostream &O;
  llvm::Function *function;
  ICFG *icfg;
  std::set<llvm::BasicBlock *> selection;

public:
  FunctionGraphWriter(llvm::raw_ostream &o, llvm::Function *func)
      : O(o), function(func) {}
  FunctionGraphWriter(llvm::raw_ostream &o, llvm::Function *func,
                      const std::set<llvm::BasicBlock *> &s)
      : O(o), function(func) {
    selection = s;
  }
  FunctionGraphWriter(llvm::raw_ostream &o, llvm::Function *func, ICFG *i,
                      const std::set<llvm::BasicBlock *> &s)
      : O(o), function(func), icfg(i) {
    selection = s;
  }

  // Writes the edge labels of the node to O and returns true if there are any
  // edge labels not equal to the empty string "".
  bool getEdgeSourceLabels(llvm::raw_ostream &o, llvm::BasicBlock *Node) {

    auto EI = llvm::succ_begin(Node);
    auto EE = llvm::succ_end(Node);
    bool hasEdgeSourceLabels = false;

    for (unsigned i = 0; EI != EE && i != 64; ++EI, ++i) {
      std::string label = getEdgeSourceLabel(Node, EI);

      if (label.empty())
        continue;

      hasEdgeSourceLabels = true;

      if (i)
        o << "|";

      o << "<s" << i << ">" << EscapeString(label);
    }

    if (EI != EE && hasEdgeSourceLabels)
      o << "|<s64>truncated...";

    return hasEdgeSourceLabels;
  }

  void writeGraph(const std::string &Title = "") {
    // Output the header for the graph...
    writeHeader(Title);

    // Emit all of the nodes in the graph...
    writeNodes();

    // Output the end of the graph
    writeFooter();
  }

  void writeHeader(const std::string &Title) {
    std::string GraphName =
        "CFG for '" + function->getName().str() + "' function";

    if (!Title.empty())
      O << "digraph \"" << EscapeString(Title) << "\" {\n";
    else if (!GraphName.empty())
      O << "digraph \"" << EscapeString(GraphName) << "\" {\n";
    else
      O << "digraph unnamed {\n";

    if (!Title.empty())
      O << "\tlabel=\"" << EscapeString(Title) << "\";\n";
    else if (!GraphName.empty())
      O << "\tlabel=\"" << EscapeString(GraphName) << "\";\n";
    O << "\n";
  }

  void writeFooter() {
    // Finish off the graph
    O << "}\n";
  }

  void writeNodes() {

    // Loop over the graph, printing it out...
    for (auto &Node : *function)
      writeNode(&Node);
  }

  std::string getNodeAttributes(llvm::BasicBlock *Node) {

    if (selection.count(Node)) {

      std::string Str;
      llvm::raw_string_ostream OS(Str);

      OS << "style=filled," << "color=green";
      return OS.str();
    }

    return "";
  }

  bool hasEdgeDestLabels() { return false; }

  unsigned numEdgeDestLabels(llvm::BasicBlock *Node) { return 0; }

  std::string getEdgeDestLabel(llvm::BasicBlock *Node, unsigned) { return ""; }

  std::string getNodeIdentifierLabel(llvm::BasicBlock *Node) { return ""; }

  std::string getNodeDescription(llvm::BasicBlock *Node) { return ""; }

  void writeNode(llvm::BasicBlock *Node) {

    std::string NodeAttributes = getNodeAttributes(Node);

    O << "\tNode" << static_cast<const void *>(Node) << " [shape=record,";
    if (!NodeAttributes.empty())
      O << NodeAttributes << ",";
    O << "label=\"{";

    O << EscapeString(getNodeLabel(Node, function));
    std::string Id = getNodeIdentifierLabel(Node);
    if (!Id.empty())
      O << "|" << EscapeString(Id);
    std::string NodeDesc = getNodeDescription(Node);
    if (!NodeDesc.empty())
      O << "|" << EscapeString(NodeDesc);

    std::string edgeSourceLabels;
    llvm::raw_string_ostream EdgeSourceLabels(edgeSourceLabels);
    bool hasEdgeSourceLabels = getEdgeSourceLabels(EdgeSourceLabels, Node);

    if (hasEdgeSourceLabels) {
      O << "|";

      O << "{" << EdgeSourceLabels.str() << "}";
    }

    if (hasEdgeDestLabels()) {
      O << "|{";

      unsigned i = 0, e = numEdgeDestLabels(Node);
      for (; i != e && i != 64; ++i) {
        if (i)
          O << "|";
        O << "<d" << i << ">" << EscapeString(getEdgeDestLabel(Node, i));
      }

      if (i != e)
        O << "|<d64>truncated...";
      O << "}";
    }

    O << "}\"];\n"; // Finish printing the "node" line

    // Output all of the edges now
    auto EI = succ_begin(Node);
    auto EE = succ_end(Node);
    for (unsigned i = 0; EI != EE && i != 64; ++EI, ++i)
      writeEdge(Node, i, EI);
    for (; EI != EE; ++EI)
      writeEdge(Node, 64, EI);
  }

  static std::string getSimpleNodeLabel(const llvm::BasicBlock *Node,
                                        const llvm::Function *) {
    if (!Node->getName().empty())
      return Node->getName().str();

    std::string Str;
    llvm::raw_string_ostream OS(Str);

    Node->printAsOperand(OS, false);
    return OS.str();
  }

  static std::string getCompleteNodeLabel(const llvm::BasicBlock *Node,
                                          const llvm::Function *) {
    enum { MaxColumns = 80 };
    std::string Str;
    llvm::raw_string_ostream OS(Str);

    if (Node->getName().empty()) {
      Node->printAsOperand(OS, false);
      OS << ":";
    }

    OS << *Node;
    std::string OutStr = OS.str();
    if (OutStr[0] == '\n')
      OutStr.erase(OutStr.begin());

    // Process string output to make it nicer...
    unsigned ColNum = 0;
    unsigned LastSpace = 0;
    for (unsigned i = 0; i != OutStr.length(); ++i) {
      if (OutStr[i] == '\n') { // Left justify
        OutStr[i] = '\\';
        OutStr.insert(OutStr.begin() + i + 1, 'l');
        ColNum = 0;
        LastSpace = 0;
      } else if (OutStr[i] == ';') {             // Delete comments!
        unsigned Idx = OutStr.find('\n', i + 1); // Find end of line
        OutStr.erase(OutStr.begin() + i, OutStr.begin() + Idx);
        --i;
      } else if (ColNum == MaxColumns) { // Wrap lines.
        // Wrap very long names even though we can't find a space.
        if (!LastSpace)
          LastSpace = i;
        OutStr.insert(LastSpace, "\\l...");
        ColNum = i - LastSpace;
        LastSpace = 0;
        i += 3; // The loop will advance 'i' again.
      } else
        ++ColNum;
      if (OutStr[i] == ' ')
        LastSpace = i;
    }
    return OutStr;
  }

  bool isSimple() { return false; }

  std::string getNodeLabel(const llvm::BasicBlock *Node,
                           const llvm::Function *Graph) {
    if (isSimple())
      return getSimpleNodeLabel(Node, Graph);
    else
      return getCompleteNodeLabel(Node, Graph);
  }

  void writeEdge(llvm::BasicBlock *Node, int edgeidx, llvm::succ_iterator EI) {

    if (llvm::BasicBlock *TargetNode = *EI) {

      int DestPort = -1;

      if (getEdgeSourceLabel(Node, EI).empty())
        edgeidx = -1;

      emitEdge(static_cast<const void *>(Node), edgeidx,
               static_cast<const void *>(TargetNode), DestPort, "");
    }
  }

  std::string getEdgeSourceLabel(const llvm::BasicBlock *Node,
                                 llvm::succ_iterator I) {
    // Label source of conditional branches with "T" or "F"
    if (const auto *BI =
            llvm::dyn_cast<llvm::BranchInst>(Node->getTerminator()))
      if (BI->isConditional())
        return (*I == Node->getTerminator()->getSuccessor(0)) ? "T" : "F";

    // Label source of switch edges with the associated value.
    if (const auto *SI =
            llvm::dyn_cast<llvm::SwitchInst>(Node->getTerminator())) {
      unsigned SuccNo = I.getSuccessorIndex();

      if (SuccNo == 0)
        return "def";

      std::string Str;
      llvm::raw_string_ostream OS(Str);
      auto Case =
          *llvm::SwitchInst::ConstCaseIt::fromSuccessorIndex(SI, SuccNo);
      OS << Case.getCaseValue()->getValue();
      return OS.str();
    }
    return "";
  }

  /// emitEdge - Output an edge from a simple node into the graph...
  void emitEdge(const void *SrcNodeID, int SrcNodePort, const void *DestNodeID,
                int DestNodePort, const std::string &Attrs) {
    if (SrcNodePort > 64)
      return; // Eminating from truncated part?
    if (DestNodePort > 64)
      DestNodePort = 64; // Targeting the truncated part?

    O << "\tNode" << SrcNodeID;
    if (SrcNodePort >= 0)
      O << ":s" << SrcNodePort;
    O << " -> Node" << DestNodeID;
    if (DestNodePort >= 0 && hasEdgeDestLabels())
      O << ":d" << DestNodePort;

    if (!Attrs.empty())
      O << "[" << Attrs << "]";
    O << ";\n";
  }

  /// getOStream - Get the raw output stream into the graph file. Useful to
  /// write fancy things using addCustomGraphFeatures().
  llvm::raw_ostream &getOStream() { return O; }
};

/// Writes graph into a provided {@code Filename}.
/// If {@code Filename} is empty, generates a random one.
/// \return The resulting filename, or an empty string if writing
/// failed.
std::string WriteGraph(llvm::Function *F, const std::string &FileName,
                       ICFG *icfg = nullptr,
                       const std::set<llvm::BasicBlock *> &selection =
                           std::set<llvm::BasicBlock *>()) {
  int FD;

  std::error_code EC = llvm::sys::fs::openFileForWrite(FileName, FD);

  // Writing over an existing file is not considered an error.
  if (EC == std::errc::file_exists) {
    llvm::errs() << "file exists, overwriting" << "\n";
  } else if (EC) {
    llvm::errs() << "error writing into file" << "\n";
    return "";
  }
  llvm::raw_fd_ostream O(FD, /*shouldClose=*/true);

  if (FD == -1) {
    llvm::errs() << "error opening file '" << FileName << "' for writing!\n";
    return "";
  }

  // Start the graph emission process...
  FunctionGraphWriter W(O, F, selection);

  // Emit the graph.
  W.writeGraph();

  return FileName;
}

/// ViewGraph - Emit a dot graph.
///
void ViewGraph(llvm::Function *F, const std::string &FileName) {
  std::string Filename = WriteGraph(F, FileName);
}

/// ViewGraph - Emit a dot graph with selection color
///
void ViewGraphWithSelection(llvm::Function *F, const std::string &FileName,
                            ICFG *icfg,
                            const std::set<llvm::BasicBlock *> &selection) {
  std::string Filename = WriteGraph(F, FileName, icfg, selection);

  if (llvm::ErrorOr<std::string> P = llvm::sys::findProgramByName("dot")) {

    std::string OutputFilename = Filename;
    if (FileName.find_last_of('.') != std::string::npos) {
      OutputFilename = FileName.substr(0, FileName.find_last_of('.')) + ".png";
    }

    std::string ProgramPath = *P;
    std::vector<llvm::StringRef> args;
    args.emplace_back("dot");
    args.emplace_back(FileName);
    args.emplace_back("-Tpng");
    args.emplace_back("-o");
    args.emplace_back(OutputFilename);
    llvm::errs() << "Trying 'dot' program " << ProgramPath << "\n";
    std::string ErrMsg;
    llvm::sys::ExecuteAndWait(ProgramPath, args, llvm::None, {}, 0, 0, &ErrMsg);
    llvm::sys::fs::remove(FileName);
  }
}
