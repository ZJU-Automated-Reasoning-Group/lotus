#include "Legacy/CFLGraph.h"

int CFLMatrix::DeleteAllEdgesAndJ(unsigned i, unsigned j) {

  int intronewAedge = 0; // is set to 1 if there are new A edges installed.
  int hasDedge = 0;

  if (i == j) {
    if (HasEdgeBetween(i, j, A)) {
      DeleteEdge(i, j, A);
    }
    if (HasEdgeBetween(i, j, Ab)) {
      DeleteEdge(i, j, Ab);
    }
    return 0;
  }

  if (HasEdgeBetween(i, j, D)) {
    hasDedge = 1;
  }

  //  i->j, every in edge should be pointed to i
  for (unsigned tmp_i = 0; tmp_i < Size; tmp_i++) {
    for (int c = 1; c < numEdgeTy; c++) {

      if (HasEdgeBetween(tmp_i, j, (EdgeTy)c)) {
        DeleteEdge(tmp_i, j, (EdgeTy)c);
        if ((c == A || c == Ab) && tmp_i == i)
          continue;
        else {
          InsertEdge(tmp_i, i, (EdgeTy)c);
          if (c == A)
            intronewAedge = 1;
          // cout<<"** insert "<<tmp_i<<" to "<<i<<'\n';
        }
      }
    }
  }

  //  j->x, every out edge should be the same as i
  for (unsigned tmp_i = 0; tmp_i < Size; tmp_i++) {
    for (int c = 1; c < numEdgeTy; c++) {

      if (HasEdgeBetween(j, tmp_i, (EdgeTy)c)) {
        DeleteEdge(j, tmp_i, (EdgeTy)c);
        if ((c == A || c == Ab) && tmp_i == i)
          continue;
        else {
          InsertEdge(i, tmp_i, (EdgeTy)c);
          if (c == A)
            intronewAedge = 1;
          // cout<<"** insert "<<tmp_i<<" to "<<i<<'\n';
        }
      }
    }
  }

  Matrix *q = p + i * Size + j;
  q->colors.reset(); // deledge from i to j;

  if (hasDedge) {
    InsertEdge(i, j, D);
    InsertEdge(j, i, Db);
  } else {

    SizeAfterDel--;
  }
  return intronewAedge;
}

unsigned CFLMatrix::FindOutDegree(unsigned i) {
  unsigned OutDegree = 0;
  for (unsigned j = 0; j < Size; j++) {
    Matrix *q = p + i * Size + j;
    OutDegree += q->colors.count();
  }
  return OutDegree;
}

unsigned CFLMatrix::FindInDegree(unsigned j) {
  unsigned InDegree = 0;
  for (unsigned i = 0; i < Size; i++) {
    Matrix *q = p + i * Size + j;
    InDegree += q->colors.count();
  }
  return InDegree;
}

void CFLMatrix::DeleteNode(unsigned i) {

  for (unsigned j = 0; j < Size; j++) {
    Matrix *q = p + i * Size + j;
    q->colors.reset();
  }

  for (unsigned j = 0; j < Size; j++) {
    Matrix *q = p + j * Size + i;
    q->colors.reset();
  }
}

long CFLMatrix::ReturnTreeNodeAndDelete() {
  //  long NodeNum = -1; //means none.

  for (unsigned i = 0; i < Size; i++) {
    if (FindOutDegree(i) == 0 && FindInDegree(i) == 0) {
      continue;
    } else if (FindOutDegree(i) == 0 && FindInDegree(i) == 1) {
      DeleteNode(i);
      return i;
    } else if (FindOutDegree(i) == 1 && FindInDegree(i) == 0) {
      DeleteNode(i);
      return i;
    }
  }
  return -1;
}