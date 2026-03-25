//===-- NissePropagation.cpp
//--------------------------------------------------===//
// Copyright (C) 2023 Leon Frenot
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the implementation of the NissePropagation
///
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <utility>

using namespace std;
using namespace llvm;

/// \brief Shorthand for long long int
using ll = long long int;
/// \brief Shorthand for vector of long long int.
using vi = vector<ll>;
/// \brief Shorthand for vector of vector of long long int.
using vvi = vector<vi>;
/// \brief Shorthand for vector of pairs of long long int.
using vpi = vector<pair<ll, ll>>;
/// \brief Shorthand for set of long long int.
using si = set<ll>;
/// \brief Shorthand for map from int to set of long long int.
using msi = map<ll, si>;

using vs = vector<string>;
using mss = map<string, si>;
using vps = vector<pair<string, string>>;

/// \brief Initialises the variables given as input with the graph described by
/// the file input.
/// \param input Path to the input file.
/// \param vertex The graph's vertices.
/// \param edges The graph's edges.
/// \param ST A spanning tree of the graph.
/// \param revST The edges not in the spanning tree.
/// \param in in[x] contains the edges towards x.
/// \param out out[x] contains the edges from x.
/// \param debug Flag for the debug messages.
void initGraph(string input, vs &vertex, vps &edges, si &ST, si &revST, mss &in,
               mss &out, bool debug) {
  int count;
  ifstream graph;
  graph.open(input + ".graph");
  graph >> count;

  if (debug)
    cout << count << '\n';

  for (int i = 0; i < count; i++) {
    string tmp;
    graph >> tmp;
    vertex.push_back(tmp);
    in[tmp] = si();
    out[tmp] = si();
  }

  if (debug) {
    for (auto i : vertex) {
      cout << i << " ";
    }
    cout << '\n';
  }

  graph >> count;
  edges = vps(count);
  for (int i = 0; i < count; i++) {
    int j;
    string a, b;
    graph >> j >> a >> b;
    edges[j] = make_pair(a, b);
    out[a].insert(j);
    in[b].insert(j);
  }

  if (debug) {
    for (auto p : edges) {
      cout << p.first << ' ' << p.second << '\n';
    }
    cout << '\n';
  }

  graph >> count;
  for (int i = 0; i < count; i++) {
    int tmp;
    graph >> tmp;
    ST.insert(tmp);
  }

  if (debug) {
    for (auto i : ST) {
      cout << i << " ";
    }
    cout << '\n';
  }

  graph >> count;
  for (int i = 0; i < count; i++) {
    int tmp;
    graph >> tmp;
    revST.insert(tmp);
  }

  if (debug) {
    for (auto i : revST) {
      cout << i << " ";
    }
    cout << '\n';
  }

  graph.close();
}

/// \brief Initialises the edge weights based on the input file. If there are
/// multiple profilings, will sum each of them to get the total profile.
/// \param input Path to the input file.
/// \param edgeCount Number of edges in the graph.
/// \param instCount Number of instrumented edges.
/// \param debug Flag for the debug messages.
/// \return The weights of the edges, initialized at 0, or at the total value
/// given in the input file.
vi initWeights(string input, vpi &prof, int edgeCount, int instCount,
               bool debug) {
  vi weights(edgeCount, 0);
  for (const auto &pair : prof) {
    const auto &edge = pair.first;
    const auto &weight = pair.second;
    weights[edge] = weight;
  }

  if (debug) {
    for (auto i : weights) {
      cout << i << " ";
    }
    cout << '\n';
  }

  return weights;
}

/// \brief Propagates the weights across the entire graph.
/// \param edges The graph's edges.
/// \param ST The graph's spanning tree (edges that have not been instrumented).
/// \param in in[x] contains the edges towards x.
/// \param out out[x] contains the edges from x.
/// \param weights The edge's weights.
/// \param v The vertex to propagate from.
/// \param e The edge to propagate from (not used on the initial call to the
/// function).
void propagation(vps &edges, si &ST, mss &in, mss &out, vi &weights, string v,
                 int e = -1) {
  ll in_sum = 0;
  for (auto ep : in[v]) {
    if (ep != e && ST.count(ep) == 1) {
      propagation(edges, ST, in, out, weights, edges[ep].first, ep);
    }
    in_sum += weights[ep];
  }

  ll out_sum = 0;
  for (auto ep : out[v]) {
    if (ep != e && ST.count(ep) == 1) {
      propagation(edges, ST, in, out, weights, edges[ep].second, ep);
    }
    out_sum += weights[ep];
  }

  if (e != -1) {
    weights[e] = max(in_sum, out_sum) - min(in_sum, out_sum);
  }
}

/// \brief Outputs the weights of the edges to the standard output.
/// \param edges The graph's edges.
/// \param weights The edge's weights.
void outputCout(vps &edges, vi &weights) {
  int size = edges.size();
  for (int i = 0; i < size; i++) {
    cout << edges[i].first << " -> " << edges[i].second << " : " << weights[i]
         << '\n';
  }
  cout << '\n';
}

/// \brief Outputs the weights of the edges to the file given as input.
/// \param filename The path to the file to write the results.
/// \param edges The graph's edges.
/// \param weights The edge's weights.
void outputFile(string filename, vps &edges, vi &weights) {
  ofstream file, bbFile;
  file.open(filename + ".edges", ios::out | ios::app);

  if (file.bad()) {
    cout << "Could not open file " << filename + ".edges" << '\n';
    outputCout(edges, weights);
  }

  int size = edges.size();
  for (int i = 0; i < size; i++) {
    file << edges[i].first << " -> " << edges[i].second << " : " << weights[i]
         << '\n';
  }

  file << '\n';
  file.close();

  bbFile.open(filename + ".bb", ios::out | ios::app);

  if (bbFile.bad()) {
    cout << "Could not open file " << filename + ".bb" << '\n';
    outputCout(edges, weights);
  }

  map<string, ll> bbFrequency;
  for (int i = 0; i < size; i++) {
    // if (edges[i].first == "0") bbFrequency["0"] += weights[i];
    bbFrequency[edges[i].second] += weights[i];
  }

  for (const auto &pair : bbFrequency) {
    const auto &bb = pair.first;
    const auto &freq = pair.second;
    bbFile << bb << " : " << freq << '\n';
  }

  bbFile << '\n';
  bbFile.close();
}

/// \brief Propagates the weights given by edge instrumentation. If the graph is
/// described in x.graph and the profiling in x.prof, call with x as the input
/// file.
/// \param argc (⊙ˍ⊙)
/// \param argv (⊙ˍ⊙)
/// \return 0
int main(int argc, char **argv) {
  cl::opt<string> InfoFilename(cl::Positional, cl::desc("<info file>"),
                               cl::Required);
  cl::opt<string> ProfFilename(cl::Positional, cl::desc("<prof file>"),
                               cl::Required);
  cl::opt<string> OutputExtension("o", cl::desc("Specify output extension"),
                                  cl::value_desc("extension"));
  cl::opt<bool> Debug("d", cl::desc("Enable debug messages"));
  cl::opt<bool> Separate(
      "s", cl::desc("Do separate profilings for each function execution"));

  cl::ParseCommandLineOptions(argc, argv);
  vector<string> functions;
  map<string, int> functionSizes;
  map<string, vpi> functionProfiles;

  {
    ifstream info_file;
    info_file.open(InfoFilename);
    string function_name;
    int sz;
    while (info_file >> function_name >> sz) {
      functions.emplace_back(function_name);
      functionSizes[function_name] = sz;
    }
    info_file.close();
  }

  {
    ifstream prof_file;
    prof_file.open(ProfFilename);
    for (auto function_name : functions) {
      auto sz = functionSizes[function_name];
      functionProfiles[function_name] = {};
      while (sz--) {
        ll idx, count;
        prof_file >> idx >> count;
        functionProfiles[function_name].emplace_back(idx, count);
      }
    }
    prof_file.close();
  }

  for (auto function_name : functions) {
    auto prof = functionProfiles[function_name];

    vs vertex;
    vps edges;
    si ST, revST;
    mss in, out;
    vvi weights;

    if (Debug) {
      cout << "\nComputing the graph of " << function_name << "\n\n";
    }

    initGraph(function_name, vertex, edges, ST, revST, in, out, Debug);

    if (Debug) {
      cout << "\nComputing the input weights\n\n";
    }

    weights.push_back(
        initWeights(function_name, prof, edges.size(), revST.size(), Debug));

    if (Debug) {
      cout << "\nPropagating the weights\n\n";
    }
    bool to_print = true;
    for (auto w : weights) {
      propagation(edges, ST, in, out, w, "0");

      if (OutputExtension.size() > 0) {
        if (to_print) {
          cout << "Writing '" << function_name << OutputExtension
               << "'... and\n";
          to_print = false;
        }
        outputFile(function_name + OutputExtension, edges, w);
      } else {
        if (to_print) {
          cout << "Printing the weights of '" << function_name << "'...\n";
          to_print = false;
        }
        outputCout(edges, w);
      }
    }
  }

  return 0;
}