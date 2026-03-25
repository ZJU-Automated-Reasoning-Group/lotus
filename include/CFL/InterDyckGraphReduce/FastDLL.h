#ifndef FASTDLL_H
#define FASTDLL_H

#include <iostream>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

using namespace std;

template <typename T1>

class In_FastDLL {
public:
  In_FastDLL() { dll_size = 0; }
  bool empty() const { return dll.empty(); }
  void add(T1 node) {

    typename list<T1>::iterator it;

    dll.push_back(node);
    it = dll.end();
    it--;
    nodemap[node] = it;
    dll_size++;
  }
  void remove(T1 node) {
    typename list<T1>::iterator it = nodemap[node];

    dll.erase(it);
    nodemap.erase(node);
    dll_size--;

    // stringmap[s] = dll.end();
  }

  int isInFDLL(T1 node) {
    if (nodemap.find(node) == nodemap.end())
      return 0;
    // if(stringmap[s] == dll.end() )
    // return 0;
    else
      return 1;
  }

  T1 front() { return dll.front(); }

  T1 front2() {
    typename list<T1>::iterator iter = dll.begin();
    iter++;
    return *iter;
  }
  void pop_front() {

    T1 dit = dll.front();
    nodemap.erase(dit);
    dll.pop_front();
    dll_size--;
  }
  unsigned size() { return dll_size; }

  void printlist() {

    cout << "==begin print list" << '\n';
    for (typename list<T1>::iterator iter = dll.begin(); iter != dll.end();
         ++iter) {
      cout << *iter << '\n';
    }
    cout << "==end" << '\n';
  }

private:
  list<T1> dll;
  unordered_map<T1, typename list<T1>::iterator> nodemap;
  unsigned dll_size;
};

#endif
