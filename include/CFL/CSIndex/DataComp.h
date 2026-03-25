#ifndef _DATA_COMPRESSION_H
#define _DATA_COMPRESSION_H

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
// #include <list>
// #include <deque>
#include <algorithm>
// #include <utility>
// #include <cmath>
// #include <unordered_map>

using namespace std;

struct mycomp {
  bool operator()(const vector<int> &_a, const vector<int> &_b) {
    if (_a.size() < _b.size())
      return true;
    else if (_a.size() == _b.size()) {
      for (int i = 0; i < _a.size(); i++) {
        if (_a[i] < _b[i])
          return true;
        else if (_a[i] > _b[i])
          return false;
      }
    }
    return false;
  }
};

class DataComp {
  vector<vector<int>> data;
  vector<vector<int>> comp_data;
  map<int, vector<int>> comp_table;
  vector<int> order;
  vector<int> classid;
  vector<vector<int>> centroids;
  int max_num;
  int num_cluster;
  int avg_length;
  int max_length;
  int threshold;
  int valid_num;
  int orgsize;

public:
  DataComp(vector<vector<int>> _data);
  DataComp(vector<vector<int>> _data, int _K);
  DataComp(vector<vector<int>> _data, vector<int> _grts, int _K, int _max);

  void getcomp_data(vector<vector<int>> &cdata);
  void getcomp_table(map<int, vector<int>> &ctable);

  void comp_kmeans();
  void init_centroids();
  void init_classid();
  void assign_class();
  void update_centroids(bool);
  void gen_result();
  int getSize();
  bool checkSize();

  void comp_swin(); // greedy heuristic based on sliding windows
  void slidewin_heu(vector<vector<int>> &pdata, vector<int> &grts,
                    map<int, vector<int>> &table, bool saved);

  void display_centroids();
  void display_compdata();
  void display_comptable();
};

#endif
