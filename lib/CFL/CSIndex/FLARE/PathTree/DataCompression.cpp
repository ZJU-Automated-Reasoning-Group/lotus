/**
 * @file DataCompression.cpp
 * @brief Implementation of data compression for reachability index storage.
 *
 * The DataCompression class implements clustering-based compression algorithms to
 * reduce the storage size of reachability indices. It uses two main approaches:
 *
 * 1. Sliding Window Heuristic (slidewin_heu):
 *    - Groups similar index vectors together
 *    - Finds common elements across vectors in a sliding window
 *    - Stores common elements once in a compression table
 *    - Each vector stores only differences from the common set
 *
 * 2. K-Means Clustering (comp_kmeans):
 *    - Clusters index vectors by similarity
 *    - Computes cluster centroids (common elements)
 *    - Stores differences from centroids for each vector
 *    - Iteratively refines clusters to minimize storage
 *
 * The compression is particularly effective when many index vectors share
 * common elements (e.g., many vertices reachable from the same gates).
 *
 * Compression format:
 *   - Original elements not in centroid
 *   - Negative indices for elements in centroid but not in vector
 *   - Cluster ID reference to centroid table
 */

#include "CFL/CSIndex/FLARE/PathTree/DataCompression.h"

#include <cstdlib>
#include <ctime>

namespace lotus::cfl::cs_index::flare::path_tree {

DataCompression::DataCompression(vector<vector<int>> _data) : data(_data) {
  srand48(time(NULL));
  num_cluster = 1;
  comp_data = vector<vector<int>>(data.size(), vector<int>());
  max_length = 0;
  int sum = 0;
  valid_num = 0;
  threshold = 4;
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    sum += data[i].size();
    if (static_cast<int>(data[i].size()) > max_length)
      max_length = data[i].size();
    if (static_cast<int>(data[i].size()) >= threshold)
      valid_num++;
  }
  avg_length = sum / data.size();
  classid = vector<int>(data.size(), -1);
  orgsize = sum;
}

DataCompression::DataCompression(vector<vector<int>> _data, vector<int> _grts, int _K,
                   int _max)
    : data(_data), order(_grts), max_num(_max), num_cluster(_K) {
  srand48(time(NULL));
  comp_data = vector<vector<int>>(data.size(), vector<int>());
  max_length = 0;
  int sum = 0;
  valid_num = 0;
  threshold = 4;
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    sum += data[i].size();
    if (static_cast<int>(data[i].size()) > max_length)
      max_length = data[i].size();
    if (static_cast<int>(data[i].size()) >= threshold)
      valid_num++;
  }
  avg_length = sum / data.size();
  classid = vector<int>(data.size(), -1);
  orgsize = sum;
}

DataCompression::DataCompression(vector<vector<int>> _data, int _K)
    : data(_data), num_cluster(_K) {
  srand48(time(NULL));
  comp_data = vector<vector<int>>(data.size(), vector<int>());
  max_length = 0;
  int sum = 0;
  valid_num = 0;
  threshold = 4;
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    sum += data[i].size();
    if (static_cast<int>(data[i].size()) > max_length)
      max_length = data[i].size();
    if (static_cast<int>(data[i].size()) >= threshold)
      valid_num++;
  }
  avg_length = sum / data.size();
  classid = vector<int>(data.size(), -1);
  orgsize = sum;
}

/**
 * @brief Sliding window heuristic for finding common elements.
 *
 * This algorithm processes vectors in a given order, maintaining a sliding
 * window of vectors and their common elements. When the common set becomes
 * too small (cost-benefit threshold), it creates a compression table entry
 * and restarts with a new window.
 *
 * Algorithm:
 * 1. Maintain a current window of vectors and their intersection
 * 2. For each new vector, check if adding it maintains sufficient commonality
 * 3. If commonality drops below threshold, compress current window and restart
 * 4. Store common elements in compression table, vectors store differences
 *
 * @param pdata Input data vectors (modified in-place if saved=true)
 * @param grts Processing order for vectors
 * @param table Output compression table mapping IDs to common element sets
 * @param saved If true, modify pdata to store compressed representation
 */
void DataCompression::slidewin_heu(vector<vector<int>> &pdata, vector<int> &grts,
                            map<int, vector<int>> &table, bool saved) {
  vector<int>::iterator vit;
  vector<int> com_set;
  vector<int> cur_cv;

  for (int i = 0; i < static_cast<int>(grts.size()); i++) {
    sort(pdata[i].begin(), pdata[i].end());
  }

  bool restart = false;
  int ind = 0, newid = -1;
  int thresh = 0, size_limit = 4;
  for (; ind < static_cast<int>(grts.size()); ind++) {
    if (static_cast<int>(pdata[grts[ind]].size()) >= size_limit - 1) {
      cur_cv.push_back(grts[ind]);
      break;
    }
  }
  while (ind < static_cast<int>(grts.size()) &&
         static_cast<int>(pdata[grts[ind]].size()) < size_limit)
    ind++;

  while (ind < static_cast<int>(grts.size())) {
    restart = false;

    if (cur_cv.size() == 1) {
      vector<int> tmp_set;
      set_intersection(pdata[cur_cv[0]].begin(), pdata[cur_cv[0]].end(),
                       pdata[grts[ind]].begin(), pdata[grts[ind]].end(),
                       back_inserter(tmp_set));
      if (tmp_set.size() >= 3) {
        com_set = tmp_set;
        cur_cv.push_back(grts[ind]);
        ind++;
        while (ind < static_cast<int>(grts.size()) &&
               static_cast<int>(pdata[grts[ind]].size()) < size_limit)
          ind++;
      } else
        restart = true;
    } else {
      vector<int> tmp_set;
      set_intersection(com_set.begin(), com_set.end(), pdata[grts[ind]].begin(),
                       pdata[grts[ind]].end(), back_inserter(tmp_set));
      if (cur_cv.size() * (com_set.size() - tmp_set.size()) <
          com_set.size() - 1 - thresh) {
        com_set = tmp_set;
        cur_cv.push_back(grts[ind]);
        ind++;
        while (ind < static_cast<int>(grts.size()) &&
               static_cast<int>(pdata[grts[ind]].size()) < size_limit)
          ind++;
      } else
        restart = true;
    }

    if (restart) {
      if (com_set.size() > 0) {
        vector<int> tmp_vec;
        table[newid] = com_set;
        for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
          tmp_vec.clear();
          set_difference(pdata[*vit].begin(), pdata[*vit].end(),
                         com_set.begin(), com_set.end(),
                         back_inserter(tmp_vec));
          if (saved) {
            pdata[*vit] = tmp_vec;
            pdata[*vit].push_back(newid);
          }
        }
        newid--;
      }

      // clear utility pdata structure
      com_set.clear();
      cur_cv.clear();
      if (ind < static_cast<int>(grts.size())) {
        cur_cv.push_back(grts[ind]);
        ind++;
        while (ind < static_cast<int>(grts.size()) &&
               static_cast<int>(pdata[grts[ind]].size()) < size_limit)
          ind++;
      }
    }
  }

  // process last cur_cv
  if (com_set.size() > 0) {
    vector<int> tmp_vec;
    table[newid] = com_set;
    for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
      vector<int>().swap(tmp_vec);
      set_difference(pdata[*vit].begin(), pdata[*vit].end(), com_set.begin(),
                     com_set.end(), back_inserter(tmp_vec));
      if (saved) {
        pdata[*vit] = tmp_vec;
        pdata[*vit].push_back(newid);
      }
    }
  }
}

void DataCompression::comp_swin() { slidewin_heu(data, order, comp_table, true); }

void DataCompression::init_classid() {
  map<int, vector<int>> table;
  vector<int>::iterator vit;
  vector<int> com_set;
  vector<int> cur_cv;

  // for test
  map<int, int> win_len;

  for (int i = 0; i < static_cast<int>(order.size()); i++) {
    sort(data[i].begin(), data[i].end());
  }

  bool restart = false;
  int ind = 0;
  int thresh = 0, size_limit = 4;
  for (; ind < static_cast<int>(order.size()); ind++) {
    if (static_cast<int>(data[order[ind]].size()) >= size_limit) {
      cur_cv.push_back(order[ind]);
      break;
    }
  }
  ind++;
  while (ind < static_cast<int>(order.size()) &&
         static_cast<int>(data[order[ind]].size()) < size_limit)
    ind++;

  int cind = 0;
  while (ind < static_cast<int>(order.size())) {
    restart = false;

    if (cur_cv.size() == 1) {
      vector<int> tmp_set;
      set_intersection(data[cur_cv[0]].begin(), data[cur_cv[0]].end(),
                       data[order[ind]].begin(), data[order[ind]].end(),
                       back_inserter(tmp_set));
      // for test
      //			cout << "check " << cur_cv[0] << " and " <<
      //order[ind] << "\n";
      if (tmp_set.size() >= 3) {
        com_set = tmp_set;
        cur_cv.push_back(order[ind]);
        ind++;
        while (ind < static_cast<int>(order.size()) &&
               static_cast<int>(data[order[ind]].size()) < size_limit)
          ind++;
      } else
        restart = true;
    } else {
      vector<int> tmp_set;
      set_intersection(com_set.begin(), com_set.end(), data[order[ind]].begin(),
                       data[order[ind]].end(), back_inserter(tmp_set));
      if (cur_cv.size() * (com_set.size() - tmp_set.size()) <
          com_set.size() - 1 - thresh) {
        com_set = tmp_set;
        cur_cv.push_back(order[ind]);
        ind++;
        while (ind < static_cast<int>(order.size()) &&
               static_cast<int>(data[order[ind]].size()) < size_limit)
          ind++;
      } else
        restart = true;
    }

    if (restart) {
      //			vector<int> tmp_vec;
      //			table[newid] = com_set;
      for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
        /*
        tmp_vec.clear();
        set_difference(data[*vit].begin(),data[*vit].end(),
                com_set.begin(),com_set.end(),back_inserter(tmp_vec));
        if (saved) {
                data[*vit] = tmp_vec;
                data[*vit].push_back(newid);
        }
        */
        classid[*vit] = cind;
      }
      cind++;
      //			newid--;

      // for test
      win_len[cur_cv.size()]++;

      // clear utility data structure
      com_set.clear();
      cur_cv.clear();
      if (ind < static_cast<int>(order.size())) {
        cur_cv.push_back(order[ind]);
        ind++;
        while (ind < static_cast<int>(order.size()) &&
               static_cast<int>(data[order[ind]].size()) < size_limit)
          ind++;
      }
    }
  }

  // process last cur_cv
  //	if (com_set.size()>0) {
  //		vector<int> tmp_vec;
  //		table[newid] = com_set;
  for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
    /*
    vector<int>().swap(tmp_vec);
    set_difference(data[*vit].begin(),data[*vit].end(),
            com_set.begin(),com_set.end(),back_inserter(tmp_vec));
    if (saved) {
            data[*vit] = tmp_vec;
            data[*vit].push_back(newid);
    }
    */
    classid[*vit] = cind;
  }
  cind++;
  //	}
  num_cluster = cind;
  centroids = vector<vector<int>>(num_cluster, vector<int>());

  win_len[cur_cv.size()]++;

  // for test
  /*
  cout << "init num clsuter = " << num_cluster << "\n";

  cout << "===========================classid=====================" << "\n";
  for (int i = 0;  i < static_cast<int>(classid.size()); i++) {
          if (static_cast<int>(data[order[i]].size())<4) continue;
          cout << "[" << order[i] << "\t" << classid[order[i]] << "] " << "\n";
  }
  cout << "\n";

  cout << "===========================window length=====================" <<
  "\n"; map<int,int>::iterator mit; for (mit = win_len.begin(); mit !=
  win_len.end(); mit++) cout << "wlen " << mit->first << " : " << mit->second <<
  "\n"; cout << "\n";
  */
}

void DataCompression::init_centroids() {
  map<int, vector<int>> tmp_table;
  slidewin_heu(data, order, tmp_table, false);

  vector<int> porder;
  vector<vector<int>> pdata;
  map<int, vector<int>>::iterator mit;
  int k = 0;
  for (k = 0, mit = tmp_table.begin(); mit != tmp_table.end(); mit++, k++) {
    pdata.push_back(mit->second);
    porder.push_back(k);
  }
  sort(pdata.begin(), pdata.end(), mycomp());
  if (static_cast<int>(pdata.size()) < num_cluster)
    num_cluster = (int)(pdata.size() * 1);

  // randomly select num_cluster centroids;
  set<int> index;
  vector<vector<int>>().swap(centroids);
  while (true) {
    int ind = lrand48() % pdata.size();
    index.insert(ind);
    if (static_cast<int>(index.size()) == num_cluster)
      break;
  }
  set<int>::iterator sit;
  for (sit = index.begin(); sit != index.end(); sit++)
    centroids.push_back(pdata[*sit]);

  // for test
  //	display_centroids();
}

void DataCompression::assign_class() {
  double distance, min_dist;
  map<int, int> cl_num;
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    if (static_cast<int>(data[i].size()) < threshold)
      continue;
    cl_num[classid[i]]++;
  }
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    if (static_cast<int>(data[i].size()) < threshold)
      continue;
    min_dist = 100000000;
    int cid = classid[i];
    for (int j = 0; j < num_cluster; j++) {
      vector<int> tmp_vec;
      set_symmetric_difference(data[i].begin(), data[i].end(),
                               centroids[j].begin(), centroids[j].end(),
                               back_inserter(tmp_vec));
      distance = tmp_vec.size() + (cl_num[cid] * 1.0) / (valid_num * 1.0) *
                                      centroids[cid].size();
      if (min_dist > distance) {
        min_dist = distance;
        classid[i] = j;
      }
    }
  }
}

void DataCompression::update_centroids(bool shrink) {
  int element, cid;
  vector<map<int, int>> vecmap = vector<map<int, int>>(num_cluster);
  vector<int> num_cl = vector<int>(num_cluster, 0);
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    if (static_cast<int>(data[i].size()) < threshold)
      continue;
    cid = classid[i];

    for (int j = 0; j < static_cast<int>(data[i].size()); j++) {
      element = data[i][j];
      vecmap[cid][element] = vecmap[cid][element] + 1;
    }
    num_cl[cid]++;
  }

  map<int, int>::iterator mit;
  for (int i = 0; i < num_cluster; i++) {
    centroids[i].clear();
    for (mit = vecmap[i].begin(); mit != vecmap[i].end(); mit++) {
      if (mit->second <= num_cl[i] * 0.5)
        continue;
      /*
      int tol = lrand48()%max_length;
      if (tol < avg_length) continue;
      */
      centroids[i].push_back(mit->first);
    }
  }

  // shrink empty centroids
  if (shrink) {
    vector<vector<int>> tmp_vec;
    for (int i = 0; i < num_cluster; i++) {
      if (static_cast<int>(centroids[i].size()) <= 0)
        continue;
      tmp_vec.push_back(centroids[i]);
    }
    centroids.clear();
    centroids = tmp_vec;
    num_cluster = tmp_vec.size();
  }

  // for test
  /*
  cout << "\n-------------------------------------------------" << "\n";
  display_centroids();
  */
}

/**
 * @brief Generate final compressed representation.
 *
 * Creates the compressed data structure:
 * 1. Build compression table: centroids stored with negative IDs
 * 2. For each vector:
 *    - Elements in vector but not in centroid: store directly
 *    - Elements in centroid but not in vector: store as -(max_num + element)
 *    - Append cluster ID (negative) to reference centroid
 *
 * Decompression: For each compressed vector, union with centroid and
 * subtract elements marked with negative indices.
 *
 * This format enables efficient storage while maintaining fast decompression.
 */
void DataCompression::gen_result() {
  int index = -1, cid = 0;

  map<int, vector<int>> cl_num;
  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    if (static_cast<int>(data[i].size()) < threshold)
      continue;
    cid = classid[i];
    cl_num[cid].push_back(i);
  }

  comp_table.clear();
  map<int, int> index_map;
  // Build compression table: store centroids with negative IDs
  for (int i = 0; i < num_cluster; i++) {
    if (static_cast<int>(cl_num[i].size()) < 2)
      continue;
    sort(centroids[i].begin(), centroids[i].end());
    comp_table[index] = centroids[i];
    index_map[i] = index;
    index--;
  }

  for (int i = 0; i < static_cast<int>(data.size()); i++) {
    if (static_cast<int>(data[i].size()) < threshold) {
      comp_data[i] = data[i];
      continue;
    }

    cid = classid[i];
    if (static_cast<int>(cl_num[cid].size()) < 2 ||
        static_cast<int>(comp_table[index_map[cid]].size()) == 0) {
      comp_data[i] = data[i];
      continue;
    }

    vector<int> tmp_vec;
    set_difference(data[i].begin(), data[i].end(), centroids[cid].begin(),
                   centroids[cid].end(), back_inserter(tmp_vec));
    comp_data[i] = tmp_vec;
    tmp_vec.clear();
    set_difference(centroids[cid].begin(), centroids[cid].end(),
                   data[i].begin(), data[i].end(), back_inserter(tmp_vec));
    for (int j = 0; j < static_cast<int>(tmp_vec.size()); j++) {
      comp_data[i].push_back(-(max_num + tmp_vec[j]));
    }
    comp_data[i].push_back(index_map[cid]);
  }
}

/**
 * @brief K-means clustering compression algorithm.
 *
 * Implements a variant of k-means clustering adapted for index compression:
 * 1. Initialize clusters using sliding window heuristic
 * 2. Assign each vector to nearest cluster (by symmetric difference)
 * 3. Update cluster centroids (common elements appearing in >50% of vectors)
 * 4. Iterate until convergence or max iterations
 * 5. Generate compressed representation
 *
 * The distance metric considers:
 * - Symmetric difference between vector and centroid
 * - Cluster size (larger clusters get preference)
 *
 * This provides better compression than sliding window alone by globally
 * optimizing cluster assignments.
 */
void DataCompression::comp_kmeans() {
  //	init_centroids();
  //	assign_class();
  init_classid();
  update_centroids(true);
  cout << "complete initialization num_cluster=" << num_cluster << "\n";

  //	if (true) exit(0);

  int iter = 0, max_iter = 5; // default is 5
  bool change = false;
  vector<int> old_cid;
  while (true && num_cluster < 15000) {
    old_cid = classid;
    cout << "begin assign class" << "\n";
    assign_class();
    cout << "complete assign class" << "\n";
    update_centroids(true);
    cout << "complete update" << "\n";

    iter++;
    change = false;
    for (int i = 0; i < num_cluster; i++) {
      if (old_cid[i] != classid[i]) {
        change = true;
        break;
      }
    }
    if (!change || iter >= max_iter)
      break;
    cout << "iter " << iter << "\t";
  }
  /*
  if (iter>0) {
  assign_class();
  update_centroids(false);
  }
  */
  gen_result();
}

int DataCompression::getSize() {
  int size = 0;
  for (int i = 0; i < static_cast<int>(comp_data.size()); i++)
    size += comp_data[i].size();
  map<int, vector<int>>::iterator mit;
  for (mit = comp_table.begin(); mit != comp_table.end(); mit++)
    size += mit->second.size();
  return size;
}

bool DataCompression::checkSize() {
  int newsize = getSize();
  return (orgsize > newsize);
}

void DataCompression::getcomp_data(vector<vector<int>> &cdata) { cdata = comp_data; }

void DataCompression::getcomp_table(map<int, vector<int>> &ctable) {
  ctable = comp_table;
}

void DataCompression::display_centroids() {
  // for test
  cout << "display centroids" << "\n";
  for (int i = 0; i < static_cast<int>(centroids.size()); i++) {
    cout << i << ": ";
    for (int j = 0; j < static_cast<int>(centroids[i].size()); j++)
      cout << centroids[i][j] << " ";
    cout << "\n";
  }
  cout << "\n";
}

void DataCompression::display_compdata() {
  cout << "Compressed data" << "\n";
  for (int i = 0; i < static_cast<int>(comp_data.size()); i++) {
    if (static_cast<int>(comp_data[order[i]].size()) == 0)
      continue;
    cout << order[i] << ": ";
    for (int j = 0; j < static_cast<int>(comp_data[order[i]].size()); j++)
      cout << comp_data[order[i]][j] << " ";
    cout << "\n";
  }
}

void DataCompression::display_comptable() {
  cout << "Coding Table comp_table size=" << comp_table.size() << "\n";
  vector<int>::iterator vit;
  map<int, vector<int>>::iterator mit;
  for (mit = comp_table.begin(); mit != comp_table.end(); mit++) {
    cout << mit->first << ": ";
    for (vit = mit->second.begin(); vit != mit->second.end(); vit++)
      cout << *vit << " ";
    cout << "\n";
  }
  cout << "\n";
}

} // namespace lotus::cfl::cs_index::flare::path_tree
