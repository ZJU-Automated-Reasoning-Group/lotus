/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s)
or their institutions liable under any circumstances.
*/
#ifndef _EXCEPTION_LIST_H
#define _EXCEPTION_LIST_H

#include <iostream>
#include <unordered_set>

namespace lotus::cfl::cs_index::flare::grail {

using namespace std;

class ExceptionList {

public:
  int size;
  unordered_set<int> *lists;
  bool LEVEL_FILTER;

public:
  ExceptionList() {}

  ~ExceptionList() { delete[] lists; }

  void set_level_filter(bool lf) { LEVEL_FILTER = lf; }
  void printExceptions() {
    unordered_set<int>::iterator siter;
    for (int node = 0; node < size; node++) {
      cout << node << " :";
      for (siter = lists[node].begin(); siter != lists[node].end(); siter++)
        cout << " " << *siter;
      cout << '\n';
    }
  }

  virtual bool isAnException(int src, int dest) {
    return lists[src].find(dest) != lists[src].end();
    // return xlists[src].find(dest) != xlists[src].end();
  }

  int getSize() {
    int i, count = 0;
    for (i = 0; i < size; i++) {
      count += lists[i].size();
    }
    return count;
  }

  void printStats() {
    int i, count = 0;
    for (i = 0; i < size; i++) {
      count += lists[i].size();
    }
    cout << " Total Number of Exceptions : " << count << '\n';
  }

  int Size() {
    int i, count = 0;
    for (i = 0; i < size; i++) {
      count += lists[i].size();
    }
    return count;
  }
};


} // namespace lotus::cfl::cs_index::flare::grail

#endif
