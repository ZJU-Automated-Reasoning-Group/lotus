#ifndef filterDkMerge_h
#define filterDkMerge_h

#include "CFL/InterDyckGraphReduce/CFLReach.h"
#include "CFL/InterDyckGraphReduce/FastDLL.h"
#include <bitset>
#include <deque>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <sys/time.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

// yuanbo modify
string edge_merge_result = "tmp_edge_merge_result.txt";
string interNd_result =
    "tmp_dkMerge_nodes_representing_cannot_remove_edges.txt";
string redmerge_result = "tmp_red_merge_result.txt";

// yuanbo modify
class MergedEdges {
public:
  unsigned afrom;
  unsigned ato;
  unsigned bfrom;
  unsigned bto;
  unsigned eid;
  MergedEdges(unsigned af, unsigned at, unsigned bf, unsigned bt, unsigned e) {
    afrom = af;
    ato = at;
    bfrom = bf;
    bto = bt;
    eid = e;
  }
};

int debug = 0;
int usearray = 1;

// should del the following two

// ifstream in("work");
// ifstream in("filelist/pldi2007/linux");

string version = "Dyck Reach";
// colorreach_graph already defined
// string colorreach_graph = "tmp_color_reach_dk_graph.dot";

// unsigned S_edge = 0;

// test
static int mergeNum = 0;

double elapsed = 0.0;

void arrayreach(CFLHashMap &cm, unordered_map<string, unsigned> &edgeStrToID,
                unordered_map<string, unsigned> &nodeStr2ID) {
  // test

  struct timeval begin, end;
  const unsigned NodeNum = cm.GetVtxNum();
  // unsigned sedges = 0;
  In_FastDLL<string> fdll;
  // typedef unordered_map<unsigned, unsigned> colorcountmap;

  unordered_map<string, In_FastDLL<unsigned>> ColorInNodes;

  unordered_map<unsigned, list<unsigned>> s_sets;

  // yuanbo
  queue<MergedEdges> mergeEdges;
  queue<string> interNodes;

  // colorcountmap colorcount;
  // vector<colorcountmap> NodeColorCount(NodeNum);
  //     unsigned edgelabels=9;

  // cout<<cm.GetEdgNum()<<'\n';

  gettimeofday(&begin, NULL);

  // preprocessing all nodes

  for (unsigned i = 0; i < NodeNum; i++) {
    // cout<<"doing"<<'\n';
    // cout<<"degree "<<cm.GetNodeDegree(i)<<'\n';

    s_sets[i].push_back(i);

    unordered_map<unsigned, Matrix1> innodes;
    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckInEdges(i, innodes);

    for (unordered_map<unsigned, Matrix1>::iterator itj = innodes.begin();
         itj != innodes.end(); ++itj) {
      unsigned j = (itj->first);
      unordered_map<unsigned, char> color = (itj->second).colors;
      // cm.CheckInColor(j, i, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {

        if (debug)
          cout << "[debug]: " << j << " " << i << " c " << (c->first) << '\n';
        // add to color in nodes
        string s;
        stringstream convert;
        convert << i << "_" << (c->first);
        s = convert.str();
        if (!ColorInNodes[s].isInFDLL(j))
          ColorInNodes[s].add(j);
      }
    }
  }

  // note: ColorInNodes is the In map in the algorithm, In[y_i] = {vertices x |
  // a_i <x, y> is an edge in G}

  // insert node if indegree > 1

  for (unordered_map<string, In_FastDLL<unsigned>>::iterator it =
           ColorInNodes.begin();
       it != ColorInNodes.end(); ++it) {
    if ((it->second).size() > 1) {
      fdll.add(it->first);
    }
  }

  // main procedure

  while (!fdll.empty()) {
    mergeNum++;
    // cout << "MergeNum is: " << mergeNum << '\n';

    const string z_string = fdll.front();

    // yuanbo modify
    string interNd;
    stringstream myss(z_string);
    myss >> interNd;
    interNodes.push(interNd);

    fdll.pop_front();
    // cout<<(fdllitem.first)<<" and "<<(fdllitem.second)<<'\n';
    if (debug) {
      // cout<<"[fdll pop] "<<z_string<<" and
      // "<<ColorInNodes[z_string].size()<<'\n';
    }

    // string s = fdllitem.first;
    /*
    unsigned z, z_color;
    size_t b;

    b = z_string.find_first_of("_");
    istringstream convert(z_string.substr(0, b));
    convert>>z;


    istringstream convert1(z_string.substr(b+1));
    convert1>>z_color;
    */

    // pick two incoming nodes x, y from z; D[x] > D[y]
    unsigned n1 = ColorInNodes[z_string].front();
    // ColorInNodes[z_string].pop_front();
    unsigned n2 = ColorInNodes[z_string].front2();
    // ColorInNodes[z_string].pop_front();
    unsigned x, y;

    if (cm.GetNodeDegree(n1) >= cm.GetNodeDegree(n2)) {
      x = n1;
      y = n2;

    } else {
      x = n2;
      y = n1;
    }
    if (debug)
      cout << "now x " << x << " y " << y << '\n';

    // append y to x and del y.
    s_sets[x].splice(s_sets[x].end(), s_sets[y]);
    s_sets.erase(y);

    // s_set[y]
    // if(cm.GetNodeDegree(y) == 0)
    // cout<<"error!"<<'\n';

    // first we handle cycle

    if (cm.HasEdgeBetween(y, y)) {

      unordered_map<unsigned, char> color;
      cm.CheckInColor(y, y, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        string sy;
        stringstream convert;
        convert << y << "_" << (c->first);
        sy = convert.str();

        // yuanbo modify
        // loop is always retained

        if (!cm.HasEdgeBetween(x, x, c->first)) {
          cm.InsertEdge(x, x, c->first);
          if (debug)
            cout << "[graph] inst: " << x << " " << x << " c " << (c->first)
                 << '\n';
          // add to color in nodes
          string s;
          stringstream convert;
          convert << x << "_" << (c->first);
          s = convert.str();
          if (debug)
            cout << "add " << s << "  " << x << '\n';

          if (!ColorInNodes[s].isInFDLL(x)) {
            ColorInNodes[s].add(x);
            /*if(ColorInNodes.find(s) == ColorInNodes.end()){
                    In_FastDLL<unsigned> ifd;
                    ifd.add(x);
                    ColorInNodes[s] = ifd;
            }else{
                    ColorInNodes[s].add(x);
                }*/

            // add x to dll if D[x] > 1
            if (debug)
              cout << "[cin] add " << s << " node " << x << '\n';
          }
          if ((ColorInNodes[s]).size() > 1) {
            if (!fdll.isInFDLL(s)) {
              fdll.add(s);
              if (debug)
                cout << "[fdll] add " << s << '\n';
            }
          }
        } else {
          // yuanbo modify
          // cout << "merge " << y << "->" << y << " to " << x << "->" << x <<
          // '\n';
          MergedEdges mEdges(y, y, x, x, c->first);
          mergeEdges.push(mEdges);
        }

        if (debug)
          cout << "[graph] del: " << y << "  " << y << " c " << (c->first)
               << '\n';
        cm.DeleteEdge(y, y, c->first);
        if (ColorInNodes[sy].isInFDLL(y)) {
          ColorInNodes[sy].remove(y);
          if (debug)
            cout << "[cin] remove " << sy << " node " << y << '\n';
        }
        if ((ColorInNodes[sy]).size() < 2) {
          if (fdll.isInFDLL(sy)) {
            fdll.remove(sy);
            if (debug)
              cout << "[fdll] remove " << sy << '\n';
          }
        }
      }
    }

    // for y 's neighbor

    unordered_map<unsigned, Matrix1> innodes;

    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckInEdges(y, innodes);

    for (unordered_map<unsigned, Matrix1>::iterator itw = innodes.begin();
         itw != innodes.end(); ++itw) {
      unsigned w = (itw->first);
      unordered_map<unsigned, char> color = (itw->second).colors;
      // cm.CheckInColor(w, y, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {

        if (!cm.HasEdgeBetween(w, x, c->first)) {
          if (debug)
            cout << "[graph] inst: " << w << " " << x << " c " << (c->first)
                 << '\n';
          cm.InsertEdge(w, x, c->first);

          // add to color in nodes
          string s;
          stringstream convert;
          convert << x << "_" << (c->first);
          s = convert.str();
          if (!ColorInNodes[s].isInFDLL(w)) {
            if (debug)
              cout << "[cin] add " << s << " node " << w << '\n';

            ColorInNodes[s].add(w);
          }
          /*if(ColorInNodes.find(s) == ColorInNodes.end()){
            In_FastDLL<unsigned> ifd;
            ifd.add(w);
            ColorInNodes[s] = ifd;
          }else{
            ColorInNodes[s].add(w);
            }*/

          // add x to dll if D[x] > 1

          if ((ColorInNodes[s]).size() > 1) {
            if (!fdll.isInFDLL(s)) {
              fdll.add(s);
              if (debug)
                cout << "[fdll] add " << s << '\n';
            }
          }
        } else {
          // yuanbo
          // cout << "Merge " << w << "->" << y << " to " << w << "->" << x <<
          // '\n';
          MergedEdges mEdges(w, y, w, x, c->first);
          mergeEdges.push(mEdges);
        }

        string sy;
        stringstream convert;
        convert << y << "_" << (c->first);
        sy = convert.str();
        if (ColorInNodes[sy].isInFDLL(w)) {
          ColorInNodes[sy].remove(w);
          if (debug)
            cout << "[cin] remove " << sy << " node " << w << '\n';
        }

        // remove w to dll if D[w] <2 1

        if ((ColorInNodes[sy]).size() < 2) {
          if (fdll.isInFDLL(sy)) {
            fdll.remove(sy);
            if (debug)
              cout << "[fdll] remove " << sy << '\n';
          }
        }
        if (debug)
          cout << "[graph] del(y in) : " << w << "  " << y << " c "
               << (c->first) << '\n';
        cm.DeleteEdge(w, y, c->first);
        // remove to color in nodes
        /*
          string s;
          stringstream convert;
          convert<<y<<"_"<<(c->first);
          s= convert.str();
          ColorInNodes[s].remove(w);
          if((ColorInNodes[s]).size() < 2)
          fdll.remove(s);
        */
      }
    }

    unordered_map<unsigned, Matrix1> outnodes;

    // colorcountmap colorcount = NodeColorCount[i];
    cm.CheckOutEdges(y, outnodes);

    for (unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin();
         itw != outnodes.end(); ++itw) {
      unsigned w = (itw->first);
      unordered_map<unsigned, char> color = (itw->second).colors;
      // cm.CheckOutColor(y, w, color);

      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); ++c) {
        string s;
        stringstream convert;
        convert << w << "_" << (c->first);
        s = convert.str();
        // cout<<"hehe "<<x<<" "<<w<< " c "<<c->first <<" has "
        // <<cm.HasEdgeBetween(x, w, c->first)<<'\n';

        if (!cm.HasEdgeBetween(x, w, c->first)) {
          if (debug)
            cout << "[graph] inst: " << x << " " << w << " c " << (c->first)
                 << '\n';
          cm.InsertEdge(x, w, c->first);

          // add to color in nodes

          if (!ColorInNodes[s].isInFDLL(x)) {
            ColorInNodes[s].add(x);
            if (debug)
              cout << "[cin] add " << s << " node " << x << '\n';
          }
          /*	    if(ColorInNodes.find(s) == ColorInNodes.end()){
            In_FastDLL<unsigned> ifd;
            ifd.add(x);
            ColorInNodes[s] = ifd;
          }else{
            ColorInNodes[s].add(x);
          }
          */

        } else {
          // yuanbo
          // cout << "Merge " << y << "->" << w << " to " << x << "->" << w <<
          // '\n';
          MergedEdges mEdges(y, w, x, w, c->first);
          mergeEdges.push(mEdges);
        }

        if (ColorInNodes[s].isInFDLL(y)) {

          if (debug)
            cout << s << "[cin] remove " << s << " node " << y << '\n';
          (ColorInNodes[s]).remove(y);
        }
        // remove w to dll if D[w] <2 1

        if ((ColorInNodes[s]).size() < 2) {
          if (fdll.isInFDLL(s)) {
            fdll.remove(s);
            if (debug)
              cout << "[fdll] remove " << s << '\n';
          }
        }
        if (debug) {
          cout << "[graph] del (y out): " << y << "  " << w << " c "
               << (c->first) << '\n';
          cout << "now " << s << " and degree " << ColorInNodes[s].size()
               << '\n';
        }
        cm.DeleteEdge(y, w, c->first);
      }
    }

    if ((ColorInNodes[z_string]).size() > 1) {
      if (!fdll.isInFDLL(z_string)) {
        fdll.add(z_string);
        if (debug)
          cout << "[fdll] add " << z_string << '\n';
      }
    }

    // cout<<"at the end: "<<z_string<<" "<<fdll.isInFDLL(z_string)<<'\n';
  }

  gettimeofday(&end, NULL);
  elapsed += ((end.tv_sec - begin.tv_sec) +
              ((end.tv_usec - begin.tv_usec) / 1000000.0));

  // cout<<"total S sets "<<s_sets.size()<<'\n';

  unsigned s_set_size = 0;
  for (unordered_map<unsigned, list<unsigned>>::iterator sit = s_sets.begin();
       sit != s_sets.end(); ++sit) {
    // cout<<"set "<<sit->first<<'\n';;
    unsigned tmps = sit->second.size();
    s_set_size = s_set_size + tmps * tmps;
    /*
    for(list<unsigned>::iterator it = (sit->second).begin(); it!=
    (sit->second).end(); ++it ) cout<<*it<<'\n';
    */
  }
  // cout<<"nodes "<<NodeNum<<'\n';
  // cout<<"s edges "<<s_set_size<<'\n';
  // yuanbo
  // produce the merged graph
  //
  unordered_map<unsigned, string> nodeid2str;
  for (unordered_map<string, unsigned>::iterator it = nodeStr2ID.begin();
       it != nodeStr2ID.end(); it++) {
    unsigned id = it->second;
    string str = it->first;
    nodeid2str[id] = str;
  }

  unordered_map<unsigned, unsigned> mergedTo;

  ofstream outNodeResult(redmerge_result);
  for (unordered_map<unsigned, list<unsigned>>::iterator sit = s_sets.begin();
       sit != s_sets.end(); sit++) {
    unsigned nid = sit->first;
    // outNodeResult << "id " << nid << " has " << sit->second.size() << "
    // nodes\n";
    outNodeResult << nodeid2str[nid] << ": ";
    for (list<unsigned>::iterator listit = sit->second.begin();
         listit != sit->second.end(); listit++) {
      mergedTo[*listit] = nid;
      outNodeResult << nodeid2str[*listit] << " ";
    }
    outNodeResult << '\n';
  }

  unordered_map<unsigned, string> edgeIDToStr;
  for (unordered_map<string, unsigned>::iterator it = edgeStrToID.begin();
       it != edgeStrToID.end(); it++) {
    string label = it->first;
    unsigned id = it->second;
    edgeIDToStr[id] = label;
  }

  // produce nodes that used as intermediate nodes, which contains unremovable
  // edge for the other color
  ofstream outInterNd(interNd_result);
  while (!interNodes.empty()) {
    string interNd = interNodes.front();
    interNodes.pop();
    stringstream ss(interNd);
    string nidstr, eidstr;
    getline(ss, nidstr, '_');
    getline(ss, eidstr, '_');
    istringstream myss1(nidstr);
    istringstream myss2(eidstr);
    int nid, eid;
    myss1 >> nid;
    outInterNd << nodeid2str[mergedTo[nid]];
    myss2 >> eid;
    outInterNd << ": " << edgeIDToStr[eid] << '\n';
  }

  ofstream out(edge_merge_result);
  while (!mergeEdges.empty()) {
    MergedEdges mEdges = mergeEdges.front();
    mergeEdges.pop();
    mEdges.afrom = mergedTo[mEdges.afrom];
    mEdges.ato = mergedTo[mEdges.ato];
    mEdges.bfrom = mergedTo[mEdges.bfrom];
    mEdges.bto = mergedTo[mEdges.bto];
    out << "Merge " << nodeid2str[mEdges.afrom] << "->"
        << nodeid2str[mEdges.ato] << " to " << nodeid2str[mEdges.bfrom] << "->"
        << nodeid2str[mEdges.bto] << ", eid " << edgeIDToStr[mEdges.eid]
        << '\n';
  }

  for (unsigned i = 0; i < NodeNum; i++) {
    unordered_map<unsigned, Matrix1> outnodes;
    cm.CheckOutEdges(i, outnodes);
    for (unordered_map<unsigned, Matrix1>::iterator itw = outnodes.begin();
         itw != outnodes.end(); itw++) {
      unsigned w = itw->first;
      unordered_map<unsigned, char> color = (itw->second).colors;
      for (unordered_map<unsigned, char>::iterator c = color.begin();
           c != color.end(); c++) {
        unsigned colorID = c->first;
        // cout << i << "->" << w << "[label=\"" << edgeIDToStr[colorID] <<
        // "\"]\n";
      }
    }
  }
}

int arrayversion() {

  // cout<<version<<'\n';
  string line;

  // string line contains the filename
  while (getline(in, line)) { // for every file

    // yuanbo modify
    line = colorreach_graph;

    unsigned NodeNum;
    SimpleDotParser dotparser;

    unordered_map<string, unsigned> NodeID;
    unordered_map<string, unsigned> EdgeID;

    // vector<CFLGrammar> CGVec;

    // cout<<"Processing "<<line<<'\n';
    // cout<<"doing "<<line<<'\n';
    NodeNum = dotparser.BuildNodeMap(line, NodeID);
    // cout<<"doing "<<line<<" of size "<<NodeNum<<'\n';
    // NodeNum = dotparser.BuildMatrix(line, NodeID);

    // CFLBitTable bt(NodeNum);
    // cout<<"node "<<NodeNum<<'\n';
    // CFLMatrix cm(NodeNum);
    // unsigned long abc = 100000;
    // cout<<"lala" << abc*abc<<'\n';
    // CFLMatrix cm(NodeNum);
    CFLHashMap cm1(NodeNum);

    /*if(NodeNum>nodemax){
      cout<<"max: "<<nodemax<<" new: "<<NodeNum<<"   "<<line<<'\n';
      nodemax = NodeNum;
      }*/

    dotparser.BuildMyHashTable(line, NodeID, EdgeID, cm1);
    // dotparser.BuildMatrix(line, NodeID, cm1);
    // dotparser.BuildBitTable(line, NodeID, cm1);

    // cout<<"Node: "<<cm1.GetVtxNum()<<" Edge "<<cm1.GetEdgNum()<<'\n';
    // cout<<"Para "<<EdgeID.size()<<'\n';

    // EdgeID.clear();
    // NodeID.clear();

    arrayreach(cm1, EdgeID, NodeID);
  }
  // SimpleDotParser dotparser;
  // dotparser.ParsingFile("./data/CINT2000/254.gap/plist.o.lala.dot");

  // cout<<"Runtime: "<<elapsed<<'\n';

  // MEM_USAGE();

  return 0;
}

int dkmerge_main() {

  // readgrammar();

  arrayversion();

  return 0;
}
#endif