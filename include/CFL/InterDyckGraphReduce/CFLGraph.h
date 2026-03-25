#ifndef CFLGRAPH_H
#define CFLGRAPH_H

#include <bitset>
//#include <deque>
#include <iostream>
//#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

// enum EdgeTy {non, D, A, M, DV, V, MAM, M_q, MA_s, MA, AM_s, AM, Ab, Db, e,
// numEdgeTy}; //note non means no edge, e means empty edge. enum EdgeTy {non,
// D, A, M, DV, V, MAM, M_q, MA_s, MA, AM_s, AM, Ab, Db, e, Dl, Dr, Vl, Vr, Vm,
// numEdgeTy}; //note non means no edge, e means empty edge. enum EdgeTy {non,
// D, A, Ab, Db, M, e, numEdgeTy};
enum EdgeTy {
  non,
  D,
  A,
  M,
  DV,
  V,
  V1,
  V2,
  V3,
  V4,
  Ab,
  Db,
  e,
  numEdgeTy
}; // note non means no edge, e means empty edge.

// enum origEdgeTy {D, A, Ab, Db, numEdgeTy}; //note non means no edge, e means
// empty edge.

// enum EdgeTy {non, D, A, M, V, Ab, Db, e, Dl, Dr, Vl, Vr, Vm, numEdgeTy};
// //note non means no edge, e means empty edge.

/*
   for edge i -> j,
p = new Matrix[n*n];
i is Matrix[i*n]
j is Matrix[i*n+j]
 */

struct Matrix {
  bitset<numEdgeTy> colors;
};

struct Matrix1 {
  // colors is a map from edge id [unsigned] -> whether contains such edge [0 or
  // 1]
  unordered_map<unsigned, char> colors;
};

class CFLMatrix {
public:
  CFLMatrix(unsigned a) : Size(a), SizeAfterDel(a) {
    cout << "a" << a << '\n';
    p = new Matrix[a * a];
  } // a*a}
  ~CFLMatrix() { delete[] p; }

  unsigned GetVtxNum() const { return Size; }
  unsigned GetSizeAfterDel() const { return SizeAfterDel; }
  unsigned GetEdgNum() {
    unsigned num = 0;
    for (unsigned i = 0; i < Size; i++) {
      for (unsigned j = 0; j < Size; j++) {
        Matrix *q = p + i * Size + j;
        num += q->colors.count();
      }
    }
    return num;
  }
  void InsertEdge(unsigned i, unsigned j, EdgeTy edgTy) {
    Matrix *q = p + i * Size + j;
    q->colors.set(edgTy);
  }
  void DeleteEdge(unsigned i, unsigned j, EdgeTy edgTy) {
    Matrix *q = p + i * Size + j;
    q->colors.reset(edgTy);
  }

  int HasEdgeBetween(unsigned i, unsigned j,
                     EdgeTy edgTy) { /*cout<<"i "<<i<<" j "<<j<<'\n';*/
    Matrix *q = p + i * Size + j;
    if (q->colors.test(edgTy))
      return 1;
    else
      return 0;
  }
  unsigned FindOutDegree(unsigned i);
  unsigned FindInDegree(unsigned j);
  void DeleteNode(unsigned i);
  long ReturnTreeNodeAndDelete();
  int DeleteAllEdgesAndJ(unsigned i,
                         unsigned j); // returns 1 if there are new A edges
                                      // installed  -- needs improvment.

private:
  Matrix *p;
  unsigned Size;
  unsigned SizeAfterDel;
};

class CFLHashMap {
public:
  CFLHashMap(unsigned a) : invec(a), outvec(a), Size(a), nodedegree(a) {
    // vector< unordered_map<unsigned, char>* > abb[a];
    // for (unsigned i = 0; i < a; ++i)
    // nodevec[i] = new unordered_map<unsigned,char>(a);

    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             invec.begin();
         iter != invec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {
      *iter = new unordered_map<unsigned, Matrix1>;
    }
    for (vector<unsigned>::iterator iter = nodedegree.begin();
         iter != nodedegree.end(); ++iter) {
      *iter = 0;
    }
  }

  ~CFLHashMap() {
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             invec.begin();
         iter != invec.end(); ++iter) {
      delete *iter;
    }
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {
      delete *iter;
    }
  }

  // delete[] p;}

  unsigned GetVtxNum() const { return Size; }
  // unsigned GetSizeAfterDel() const {return SizeAfterDel;}
  unsigned GetEdgNum() {
    unsigned num = 0;
    // for (unsigned i = 0; i< Size; i++){
    for (vector<unordered_map<unsigned, Matrix1> *>::iterator iter =
             outvec.begin();
         iter != outvec.end(); ++iter) {

      for (unordered_map<unsigned, Matrix1>::iterator j = (**iter).begin();
           j != (**iter).end(); ++j) {
        num += j->second.colors.size();
      }
    }
    return num;
    //}
  }

  void InsertEdge(unsigned i, unsigned j, unsigned edgelabel) {
    if (outvec[i]->count(j) <= 0) { // can't find to
      Matrix1 tmp_q;
      tmp_q.colors[edgelabel] = 1;
      (*outvec[i])[j] = tmp_q;
    } else {
      (*outvec[i])[j].colors[edgelabel] = 1;
    }

    if (invec[j]->count(i) <= 0) { // can't find to
      Matrix1 tmp_q;
      tmp_q.colors[edgelabel] = 1;
      (*invec[j])[i] = tmp_q;
    } else {
      (*invec[j])[i].colors[edgelabel] = 1;
    }
    nodedegree[i]++;
    nodedegree[j]++;
  }

  void DeleteEdge(unsigned i, unsigned j, unsigned edgelabel) {
    (*outvec[i])[j].colors.erase(edgelabel);
    (*invec[j])[i].colors.erase(edgelabel);
    nodedegree[i]--;
    nodedegree[j]--;
  }

  int HasEdgeBetween(unsigned i, unsigned j) {

    if (outvec[i]->count(j) <= 0) { // can't find to
      return 0;
    } else {
      return 1;
    }
  }

  int HasEdgeBetween(unsigned i, unsigned j, unsigned edgelabel) {

    if (outvec[i]->count(j) <= 0) { // can't find to
      return 0;
    } else {

      if ((*outvec[i])[j].colors.count(edgelabel))
        return 1;
      else
        return 0;
    }
  }

  void CheckOutEdges(unsigned i, unordered_map<unsigned, Matrix1> &outnodes) {

    outnodes = (*outvec[i]);
  }

  void CheckInEdges(unsigned i, unordered_map<unsigned, Matrix1> &innodes) {

    innodes = (*invec[i]);
  }

  void CheckInColor(unsigned j, unsigned i, unordered_map<unsigned, char> &c) {
    c = (*invec[i])[j].colors;
  }
  void CheckOutColor(unsigned i, unsigned j, unordered_map<unsigned, char> &c) {
    c = (*outvec[i])[j].colors;
  }
  // unsigned FindOutDegree(unsigned i);
  // unsigned FindInDegree(unsigned j);
  // void DeleteNode(unsigned i);
  // long ReturnTreeNodeAndDelete();
  // int DeleteAllEdgesAndJ(unsigned i, unsigned j); //returns 1 if there are
  // new A edges installed  -- needs improvment.
  unsigned GetNodeDegree(unsigned node) { return nodedegree[node]; }

private:
  vector<unordered_map<unsigned, Matrix1> *> invec;
  vector<unordered_map<unsigned, Matrix1> *> outvec;
  unsigned Size;
  vector<unsigned> nodedegree;
  // unsigned SizeAfterDel;
};

#endif
