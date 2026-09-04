#ifndef _PATHTREE_QUERY_H_
#define _PATHTREE_QUERY_H_

#include "CFL/CSIndex/FLARE/Index.h"

namespace lotus::cfl::cs_index::flare::path_tree {

// #define PATHTREE_DEBUG
#define PATHTREE_CHECK

using namespace std;

struct vertexlabel {
  int preorder;
  int postorder;
  int begin;
  int size;
};
struct dfssorter {
  vector<vector<int>> *labels;
  dfssorter(vector<vector<int>> *_labels) : labels(_labels) {}
  bool operator()(const int a, const int b) const {
    if ((*labels)[a][2] < (*labels)[b][2])
      return true;
    return false;
  }
};
struct sorter {
  bool reverse;
  vector<int> *labels;
  sorter(vector<int> *_labels, bool _r) : labels(_labels), reverse(_r) {}
  bool operator()(const int a, const int b) const {
    if (!reverse) {
      if ((*labels)[a] > (*labels)[b])
        return true;
      return false;
    }

    if ((*labels)[a] < (*labels)[b])
      return true;
    return false;
  }
};

class Query : public flare::Index {
private:
  vector<int> topoid;
  int *out_uncover;
  vector<int> dfsmap;
  vector<vertexlabel> vlabels;

  // for statistics
  int totalingates, qnum, checkoutgates, comparenum, invisit, outvisit;

public:
  Query(const char *gatefile, const char *ggfile, const char *indexfile,
                const char *grafile)
      : flare::Index(gatefile, ggfile, indexfile, grafile) {
    dim = 5;
    method_name = "PATHTREE";
    GraphAlgorithms::topological_sort(g, topoid);
    // for original graph labeling
    cout << "GRAIL labeling dim=" << dim << '\n';
    computeMultiLabelsQuickRej(dim); // actually call mutipleLabeling()
    initMaterialization();
    sortLoutDFSorder();
    cout << "converting labels..." << '\n';
    labelconversion();

#ifdef PATHTREE_DEBUG
    displayIndex(cout);
    displayLabels(cout);
#endif

    // for statistics
    qnum = 0;
    totalingates = 0;
    checkoutgates = 0;
    comparenum = 0;
    invisit = 0;
    outvisit = 0;
  }

  Query(const char *filestem, const char *grafile, int _r, double _ps,
                bool mat)
      : flare::Index(filestem, grafile, _r, _ps, mat) {
    dim = 5;
    method_name = "PATHTREE";
    GraphAlgorithms::topological_sort(g, topoid);
    // for original graph labeling
    cout << "GRAIL labeling dim=" << dim << '\n';
    struct timeval after_time, before_time;
    float query_time = 0;
    gettimeofday(&before_time, NULL);
    computeMultiLabelsQuickRej(dim); // actually call mutipleLabeling()
    if (mat) {
      cout << "init materialization ..." << '\n';
      initMaterialization();
    }
    gettimeofday(&after_time, NULL);
    query_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
                 (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
    cout << "#GRAIL labeling time:" << query_time << " (ms)" << '\n';
    cout << "sort lout DFS order ..." << '\n';
    sortLoutDFSorder();
    cout << "converting labels..." << '\n';
    labelconversion();
    cout << " done " << '\n';
#ifdef PATHTREE_DEBUG
    displayIndex(cout);
    displayLabels(cout);
#endif

    // for statistics
    qnum = 0;
    totalingates = 0;
    checkoutgates = 0;
    comparenum = 0;
    invisit = 0;
    outvisit = 0;
  }

  Query(const char *filestem, Graph &ig, int _r, double _ps, bool mat,
                double *grail_label_duration)
      : flare::Index(filestem, ig, _r, _ps, mat) {
    dim = grail_dim;
    method_name = "PATHTREE";
    GraphAlgorithms::topological_sort(g, topoid);
    // for original graph labeling
    //		cout << "GRAIL labeling dim=" << dim << '\n';
    struct timeval after_time, before_time;
    float query_time = 0;
    gettimeofday(&before_time, NULL);
    computeMultiLabelsQuickRej(dim); // actually call mutipleLabeling()
    if (mat) {
      initMaterialization();
    }
    gettimeofday(&after_time, NULL);
    query_time = (after_time.tv_sec - before_time.tv_sec) * 1000.0 +
                 (after_time.tv_usec - before_time.tv_usec) * 1.0 / 1000.0;
    *grail_label_duration = query_time;
    //		cout << "#GRAIL labeling time:" << query_time << " (ms)" <<
    //'\n';
    sortLoutDFSorder();
    labelconversion();
#ifdef PATHTREE_DEBUG
    displayIndex(cout);
    displayLabels(cout);
#endif

    // for statistics
    qnum = 0;
    totalingates = 0;
    checkoutgates = 0;
    comparenum = 0;
    invisit = 0;
    outvisit = 0;
  }

  ~Query() {
    delete out_uncover;
    cout << "average ingates size=" << (1.0 * totalingates) / (qnum * 1.0)
         << '\n';
    cout << "average checkougates size=" << (1.0 * checkoutgates) / (qnum * 1.0)
         << '\n';
    cout << "average comparenum=" << (1.0 * comparenum) / (1.0 * qnum) << '\n';
    cout << "average invisit=" << (1.0 * invisit) / (1.0 * qnum) << '\n';
    cout << "average outvisit=" << (1.0 * outvisit) / (1.0 * qnum) << '\n';
  }

  void labelconversion() {
    int totalsize = 0;
    dfsmap = vector<int>(gsize, -1);
    for (int i = 0; i < gsize; i++) {
      if (!gates->get(i))
        continue;
      dfsmap[i] = labels[i][2];
      vlabels.push_back(vertexlabel());
      totalsize += lout[i].size();
    }
    int prev = 0, counter = 0;
    out_uncover = new int[totalsize];
    for (int i = 0; i < gsize; i++) {
      if (!gates->get(i))
        continue;
      vertexlabel vl;
      vl.preorder = labels[i][0];
      vl.postorder = labels[i][1];
      vl.begin = prev;
      vl.size = lout[i].size();
      vlabels[dfsmap[i]] = vl;
      prev += lout[i].size();
      for (int j = 0; j < lout[i].size(); j++) {
        out_uncover[counter++] = dfsmap[lout[i][j]];
      }
    }
  }

  void sortLoutDFSorder() {
    for (int i = 0; i < gsize; i++)
      sort(lout[i].begin(), lout[i].end(), dfssorter(&labels));
  }

  // return the number of intervals in tree labeling
  long getIndexSize() const {
    long labelsize = 0;
    for (int i = 0; i < gsize; i++) {
      labelsize += lout[i].size();
    }
    return labelsize;
  }

  void displayInfor(int vid, ostream &out) {
    if (materialized->get(vid)) {
      if (inneigs[vid] == NULL) {
        cout << "Error: " << vid << " is NULL" << '\n';
        return;
      }
      out << "Inneigs[" << vid << "]: ";
      int count = 0;
      for (int i = 0; i < gsize; i++)
        if (inneigs[vid]->get(i)) {
          out << i << " ";
          count++;
          if (count > 200) {
            out << "too long";
            break;
          }
        }
      out << '\n';
    }
    out << "InOutGates[" << vid << "]: ";
    vector<int>::iterator vit;
    if (inoutgates[vid][0].size() > 200)
      out << "too long";
    else {
      for (vit = inoutgates[vid][0].begin(); vit != inoutgates[vid][0].end();
           vit++)
        out << *vit << "(" << topoid[*vit] << ")" << " ";
    }
    out << " | ";
    if (inoutgates[vid][1].size() > 200)
      out << "too long";
    else {
      for (vit = inoutgates[vid][1].begin(); vit != inoutgates[vid][1].end();
           vit++)
        out << *vit << "(" << topoid[*vit] << ")" << " ";
    }
    out << "#" << '\n';
  }

  void displayLabelsByNode(int vid, ostream &out) {
    vector<int>::iterator vit;
    if (gates->get(vid))
      out << vid << ": [" << labels[vid][0] << "," << labels[vid][1] << ","
          << labels[vid][2] << "]" << '\n';
    else
      out << vid << ": NULL" << '\n';
    cout << "Lout[" << vid << "]: ";
    for (vit = lout[vid].begin(); vit != lout[vid].end(); vit++)
      cout << *vit << "(" << labels[vid][2] << ") ";
    cout << '\n';
    if (ismaterialized)
      displayInfor(vid, out);
  }

  void mutipleLabeling(int num_labels) {
    int index = 0;
    vector<pair<int, int>> dfslabels;
    graillabels =
        vector<vector<pair<int, int>>>(gsize, vector<pair<int, int>>());
    for (int i = 0; i < num_labels; i++) {
      dfslabels.clear();
      //	GraphAlgorithms::randomDFSLabeling(g,dfslabels);
      GraphAlgorithms::grail_labeling(g, dfslabels);
      for (int j = 0; j < gsize; j++) {
        graillabels[j].push_back(dfslabels[j]);
      }
    }
    useGlobalMultiLabels = true;
  }

  // check reachability between src and trg using global index from
  // computeMultiLabelsQuickRej
  inline bool contains(int src, int trg) {
    for (int i = 0; i < dim; i++) {
      if (graillabels[src][i].first > graillabels[trg][i].first)
        return false;
      if (graillabels[src][i].second < graillabels[trg][i].second)
        return false;
    }
    return true;
  }

  // query version using materalized data and bidirectional BFS
  bool reach(int src, int trg) {
#ifdef PATHTREE_DEBUG
    cout << "check " << src << "->" << trg << '\n';
#endif
    if (src == trg)
      return true;
    if (!contains(src, trg))
      return false;

    QueryCnt++;
    qnum++;
    vector<int> ingates;
    vector<int>::iterator outiter, initer;
    int u, val, index = 0, endindex = 0, nid, fradius, bradius;
    EdgeList el;
    EdgeList::iterator eit;

    if (materialized->get(trg)) {
      if (inneigs[trg]->get(src))
        return true;
      if (!gates->get(trg) && inoutgates[trg][0].empty())
        return false;
    } else if (!gates->get(trg) && !gates->get(src)) {
      // check local vertices using bidirectional search
      fradius = (radius) / 2;
      bradius = radius - fradius;
      // perform forward search from src
      ref += radius + 1;
      que[0] = src;
      dist[src] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.out_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            if (nid == trg)
              return true;
            dist[nid] = val + 1;
            if (!contains(nid, trg))
              continue;
            visited[nid] = QueryCnt;
            if (gates->get(nid))
              continue;
            if (val + 1 - ref < fradius)
              que[endindex++] = nid;
          }
        }
      }
      // perform backward search
      ref += radius + 1;
      que[0] = trg;
      dist[trg] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.in_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            if (nid == src || visited[nid] == QueryCnt)
              return true;
            dist[nid] = val + 1;
            if (!contains(src, nid))
              continue;
            if (gates->get(nid))
              continue;
            if (val + 1 - ref < bradius)
              que[endindex++] = nid;
          }
        }
      }
    }

    // consider different cases
    if (gates->get(src)) {
      if (gates->get(trg)) {
        // both src and trg are gates
        reachtime++;
        int srcdfsid = dfsmap[src];
        int trgdfsid = dfsmap[trg];
        int post2 = vlabels[trgdfsid].postorder;
        if (srcdfsid < trgdfsid && post2 >= vlabels[srcdfsid].preorder &&
            post2 <= vlabels[srcdfsid].postorder)
          return true;
        int size = vlabels[srcdfsid].size;
        if (size == 0)
          return false;
        int *pointer = out_uncover + vlabels[srcdfsid].begin;
        int *end = pointer + size;
        int index;
        for (; pointer < end; pointer++) {
          index = *pointer;
          if (index > trgdfsid)
            return false;
          if (post2 >= vlabels[index].preorder &&
              post2 <= vlabels[index].postorder)
            return true;
        }
      } else {
        // src is gate and trg is NOT gate
        int srcdfsid = dfsmap[src], trgdfsid, post2, begin;
        int pre1 = vlabels[srcdfsid].preorder;
        int post1 = vlabels[srcdfsid].postorder;
        int size = vlabels[srcdfsid].size;
        int *pointer = 0, *end = 0;
        if (size == 0) {
          for (initer = inoutgates[trg][0].begin();
               initer != inoutgates[trg][0].end(); initer++) {
            trgdfsid = dfsmap[*initer];
            post2 = vlabels[trgdfsid].postorder;
            if (srcdfsid <= trgdfsid && post2 >= pre1 && post2 <= post1)
              return true;
          }
        } else {
          int index;
          begin = vlabels[srcdfsid].begin;
          for (initer = inoutgates[trg][0].begin();
               initer != inoutgates[trg][0].end(); initer++) {
            if (!contains(src, *initer))
              continue;
            reachtime++;
            trgdfsid = dfsmap[*initer];
            post2 = vlabels[trgdfsid].postorder;
            if (srcdfsid <= trgdfsid && post2 >= pre1 && post2 <= post1)
              return true;
            pointer = out_uncover + begin;
            end = pointer + size;
            for (; pointer < end; pointer++) {
              index = *pointer;
              if (index > trgdfsid)
                break;
              if (post2 >= vlabels[index].preorder &&
                  post2 <= vlabels[index].postorder)
                return true;
            }
          }
        }
      }
    } else {
      if (gates->get(trg)) {
        // src is NOT gate and trg is gate
        int srcdfsid, post1, begin, pre1;
        int trgdfsid = dfsmap[trg];
        int post2 = vlabels[trgdfsid].postorder;
        int size, index;
        int *pointer = 0, *end = 0;

        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          reachtime++;
          srcdfsid = dfsmap[nid];
          pre1 = vlabels[srcdfsid].preorder;
          post1 = vlabels[srcdfsid].postorder;
          if (srcdfsid <= trgdfsid && post2 >= pre1 && post2 <= post1)
            return true;
          size = vlabels[srcdfsid].size;
          if (size == 0)
            continue;
          pointer = out_uncover + vlabels[srcdfsid].begin;
          end = pointer + size;
          for (; pointer < end; pointer++) {
            index = *pointer;
            if (index > trgdfsid)
              break;
            if (post2 >= vlabels[index].preorder &&
                post2 <= vlabels[index].postorder)
              return true;
          }
        }
      } else {
        // both src and trg are NOT gates
        int srcdfsid, post1, begin, pre1;
        int trgdfsid, post2;
        int size, index;
        int *pointer = 0, *end = 0;

        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          srcdfsid = dfsmap[nid];
          pre1 = vlabels[srcdfsid].preorder;
          post1 = vlabels[srcdfsid].postorder;
          size = vlabels[srcdfsid].size;
          for (initer = inoutgates[trg][0].begin();
               initer != inoutgates[trg][0].end(); initer++) {
            reachtime++;
            trgdfsid = dfsmap[*initer];
            post2 = vlabels[trgdfsid].postorder;
            if (srcdfsid <= trgdfsid && post2 >= pre1 && post2 <= post1)
              return true;
            if (size == 0)
              continue;
            pointer = out_uncover + vlabels[srcdfsid].begin;
            end = pointer + size;
            for (; pointer < end; pointer++) {
              index = *pointer;
              if (index > trgdfsid)
                break;
              if (post2 >= vlabels[index].preorder &&
                  post2 <= vlabels[index].postorder)
                return true;
            }
          }
        }
      }
    }

    return false;
  }

  // query version using materalized data
  bool reachWithoutBiBFS(int src, int trg) {
#ifdef PATHTREE_DEBUG
    cout << "check " << src << "->" << trg << '\n';
#endif
    if (src == trg)
      return true;
    if (!contains(src, trg))
      return false;

    QueryCnt++;
    qnum++;
    vector<int> ingates;
    vector<int>::iterator outiter, initer;
    int u, val, index = 0, endindex = 0, nid;
    bool check = false;
    EdgeList el;
    EdgeList::iterator eit;

    if (materialized->get(trg)) {
      if (inneigs[trg]->get(src))
        return true;
      if (!gates->get(trg) && inoutgates[trg][0].empty())
        return false;
    } else if (!gates->get(trg)) {
      // check local vertices
      ref += radius + 1;
      que[0] = trg;
      dist[trg] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.in_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            invisit++;
            if (nid == src)
              return true;
            dist[nid] = val + 1;
            if (!contains(src, nid))
              continue;
            if (gates->get(nid)) {
              //	visited[nid] = QueryCnt;
              //	ingates.push_back(nid);
              check = true;
              continue;
            }
            if (val + 1 - ref < radius) {
              que[endindex++] = nid;
            }
          }
        }
      }
      if (!check)
        return false;
    }

    // consider different cases
    if (gates->get(src)) {
      if (gates->get(trg)) {
        // both src and trg are gates
        reachtime++;
        if (labels[src][2] <= labels[trg][2]) {
          comparenum++;
          if (labels[src][0] <= labels[trg][0] &&
              labels[src][1] >= labels[trg][1])
            return true;
        }
        for (outiter = lout[src].begin(); outiter != lout[src].end();
             outiter++) {
          comparenum++;
          if (labels[*outiter][2] > labels[trg][2])
            break;
          if (labels[*outiter][0] <= labels[trg][0] &&
              labels[*outiter][1] >= labels[trg][1])
            return true;
        }
      } else {
        // src is gate and trg is NOT gate
        for (initer = inoutgates[trg][0].begin();
             initer != inoutgates[trg][0].end(); initer++) {
          if (!contains(src, *initer))
            continue;
          reachtime++;
          if (labels[src][2] <= labels[*initer][2]) {
            comparenum++;
            if (labels[src][0] <= labels[*initer][0] &&
                labels[src][1] >= labels[*initer][1])
              return true;
          }
          for (outiter = lout[src].begin(); outiter != lout[src].end();
               outiter++) {
            comparenum++;
            if (labels[*outiter][2] > labels[*initer][2])
              break;
            if (labels[*outiter][0] <= labels[*initer][0] &&
                labels[*outiter][1] >= labels[*initer][1])
              return true;
          }
        }
      }
    } else {
      if (gates->get(trg)) {
        // src is NOT gate and trg is gate
        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          reachtime++;
          if (labels[nid][2] <= labels[trg][2]) {
            comparenum++;
            if (labels[nid][0] <= labels[trg][0] &&
                labels[nid][1] >= labels[trg][1])
              return true;
          }
          for (outiter = lout[nid].begin(); outiter != lout[nid].end();
               outiter++) {
            comparenum++;
            if (labels[*outiter][2] > labels[trg][2])
              break;
            if (labels[*outiter][0] <= labels[trg][0] &&
                labels[*outiter][1] >= labels[trg][1])
              return true;
          }
        }
      } else {
        // both src and trg are NOT gates
        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          for (initer = inoutgates[trg][0].begin();
               initer != inoutgates[trg][0].end(); initer++) {
            reachtime++;
            if (labels[nid][2] <= labels[*initer][2]) {
              comparenum++;
              if (labels[nid][0] <= labels[*initer][0] &&
                  labels[nid][1] >= labels[*initer][1])
                return true;
            }
            for (outiter = lout[nid].begin(); outiter != lout[nid].end();
                 outiter++) {
              comparenum++;
              if (labels[*outiter][2] > labels[*initer][2])
                break;
              if (labels[*outiter][0] <= labels[*initer][0] &&
                  labels[*outiter][1] >= labels[*initer][1])
                return true;
            }
          }
        }
      }
    }

    return false;
  }

  // query version based on including high indegree vertices in the gate graph
  bool reachIndegGate(int src, int trg) {
#ifdef PATHTREE_DEBUG
    cout << "check " << src << "->" << trg << '\n';
#endif
    if (src == trg)
      return true;
    if (!contains(src, trg))
      return false;

    QueryCnt++;
    qnum++;
    vector<int> ingates;
    vector<int>::iterator outiter, initer;
    int u, val, index = 0, endindex = 0, nid;
    bool check = false;
    EdgeList el;
    EdgeList::iterator eit;

#ifdef PATHTREE_CHECK
    if (gates->get(trg)) {
      visited[trg] = QueryCnt;
      ingates.push_back(trg);
      check = true;
      totalingates++;
    } else {
#endif
      ref += radius + 1;
      que[0] = trg;
      dist[trg] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.in_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            invisit++;
            if (nid == src)
              return true;
            dist[nid] = val + 1;
            if (!contains(src, nid))
              continue;
            if (gates->get(nid)) {
              visited[nid] = QueryCnt;
              ingates.push_back(nid);
              check = true;
              totalingates++;
#ifdef PATHTREE_DEBUG
              cout << "ingate=" << nid << '\n';
#endif
              continue;
            }
            if (val + 1 - ref < radius) {
              que[endindex++] = nid;
            }
          }
        }
      }
#ifdef PATHTREE_CHECK
    }
#endif

    if (!check)
      return false;

#ifdef PATHTREE_CHECK
    if (gates->get(src)) {
      checkoutgates++;
      for (initer = ingates.begin(); initer != ingates.end(); initer++) {
        if (!contains(src, *initer))
          continue;
        if (labels[src][2] <= labels[*initer][2]) {
          comparenum++;
          if (labels[src][0] <= labels[*initer][0] &&
              labels[src][1] >= labels[*initer][1])
            return true;
        }
        for (outiter = lout[src].begin(); outiter != lout[src].end();
             outiter++) {
          if (visited[*outiter] == QueryCnt)
            return true;
          comparenum++;
          if (labels[*outiter][2] > labels[*initer][2])
            continue;
          if (labels[*outiter][0] <= labels[*initer][0] &&
              labels[*outiter][1] >= labels[*initer][1])
            return true;
        }
      }
    } else {
#endif
      for (int i = 0; i < inoutgates[src][1].size(); i++) {
        nid = inoutgates[src][1][i];
        if (!contains(nid, trg))
          continue;
        for (initer = ingates.begin(); initer != ingates.end(); initer++) {
          if (labels[nid][2] <= labels[*initer][2]) {
            comparenum++;
            if (labels[nid][0] <= labels[*initer][0] &&
                labels[nid][1] >= labels[*initer][1])
              return true;
          }
          for (outiter = lout[nid].begin(); outiter != lout[nid].end();
               outiter++) {
            comparenum++;
            if (labels[*outiter][2] > labels[*initer][2])
              continue;
            if (labels[*outiter][0] <= labels[*initer][0] &&
                labels[*outiter][1] >= labels[*initer][1])
              return true;
          }
        }
      }
#ifdef PATHTREE_CHECK
    }
#endif

    return false;
  }

  // query version without sorting
  bool reachWithoutMat(int src, int trg) {
#ifdef PATHTREE_DEBUG
    cout << "check " << src << "->" << trg << '\n';
#endif
    if (src == trg)
      return true;
    if (!contains(src, trg))
      return false;

    QueryCnt++;
    qnum++;
    vector<int> ingates;
    vector<int>::iterator outiter, initer;
    int u, val, index = 0, endindex = 0, nid;
    bool check = false;
    EdgeList el;
    EdgeList::iterator eit;

#ifdef PATHTREE_CHECK
    if (gates->get(trg)) {
      visited[trg] = QueryCnt;
      ingates.push_back(trg);
      check = true;
      totalingates++;
    } else {
#endif
      ref += radius + 2;
      que[0] = trg;
      dist[trg] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.in_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            invisit++;
            if (nid == src)
              return true;
            dist[nid] = val + 1;
            if (!contains(src, nid))
              continue;
            if (gates->get(nid)) {
              visited[nid] = QueryCnt;
              ingates.push_back(nid);
              check = true;
              totalingates++;
#ifdef PATHTREE_DEBUG
              cout << "ingate=" << nid << '\n';
#endif
              continue;
            }
            if (val + 1 - ref < radius) {
              que[endindex++] = nid;
            }
          }
        }
      }
#ifdef PATHTREE_CHECK
    }
#endif

    if (!check)
      return false;

#ifdef PATHTREE_CHECK
    if (gates->get(src)) {
      checkoutgates++;
      for (initer = ingates.begin(); initer != ingates.end(); initer++) {
        if (labels[src][2] <= labels[*initer][2]) {
          comparenum++;
          if (labels[src][0] <= labels[*initer][0] &&
              labels[src][1] >= labels[*initer][1])
            return true;
        }
        for (outiter = lout[src].begin(); outiter != lout[src].end();
             outiter++) {
          if (visited[*outiter] == QueryCnt)
            return true;
          comparenum++;
          if (labels[*outiter][2] > labels[*initer][2])
            continue;
          if (labels[*outiter][0] <= labels[*initer][0] &&
              labels[*outiter][1] >= labels[*initer][1])
            return true;
        }
      }
    } else {
#endif
      ref += radius + 2;
      que[0] = src;
      dist[src] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.out_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            dist[nid] = val + 1;
            if (!contains(nid, trg))
              continue;
            if (gates->get(nid)) {
              outvisit++;
              if (visited[nid] == QueryCnt)
                return true;
#ifdef PATHTREE_DEBUG
              cout << "outgate=" << nid << '\n';
#endif
              checkoutgates++;
              for (initer = ingates.begin(); initer != ingates.end();
                   initer++) {
                if (labels[nid][2] <= labels[*initer][2]) {
                  comparenum++;
                  if (labels[nid][0] <= labels[*initer][0] &&
                      labels[nid][1] >= labels[*initer][1])
                    return true;
                }
                for (outiter = lout[nid].begin(); outiter != lout[nid].end();
                     outiter++) {
                  comparenum++;
                  if (labels[*outiter][2] > labels[*initer][2])
                    continue;
                  if (labels[*outiter][0] <= labels[*initer][0] &&
                      labels[*outiter][1] >= labels[*initer][1])
                    return true;
                }
              }
              continue;
            }
            if (val + 1 - ref < radius) {
              que[endindex++] = nid;
            }
          }
        }
      }
#ifdef PATHTREE_CHECK
    }
#endif

    return false;
  }

  bool reachBeforeImprovement(int src, int trg) {
#ifdef PATHTREE_DEBUG
    cout << "check " << src << "->" << trg << '\n';
#endif
    if (src == trg)
      return true;
    if (!contains(src, trg))
      return false;

    QueryCnt++;
    qnum++;
    vector<int> ingates;
    vector<int>::iterator outiter, initer;
    int u, val, index = 0, endindex = 0, nid, fradius, bradius;
    EdgeList el;
    EdgeList::iterator eit;

    if (materialized->get(trg)) {
      if (inneigs[trg]->get(src))
        return true;
      if (!gates->get(trg) && inoutgates[trg][0].empty())
        return false;
    } else if (!gates->get(trg) && !gates->get(src)) {
      // check local vertices using bidirectional search
      fradius = (radius) / 2;
      bradius = radius - fradius;
      // perform forward search from src
      ref += radius + 1;
      que[0] = src;
      dist[src] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.out_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            if (nid == trg)
              return true;
            dist[nid] = val + 1;
            if (!contains(nid, trg))
              continue;
            visited[nid] = QueryCnt;
            if (gates->get(nid))
              continue;
            if (val + 1 - ref < fradius)
              que[endindex++] = nid;
          }
        }
      }
      // perform backward search
      ref += radius + 1;
      que[0] = trg;
      dist[trg] = ref;
      endindex = 1;
      index = 0;
      while (index < endindex) {
        u = que[index];
        index++;
        val = dist[u];
        el = g.in_edges(u);
        for (eit = el.begin(); eit != el.end(); eit++) {
          nid = (*eit);
          if (dist[nid] < ref) {
            if (nid == src || visited[nid] == QueryCnt)
              return true;
            dist[nid] = val + 1;
            if (!contains(src, nid))
              continue;
            if (gates->get(nid))
              continue;
            if (val + 1 - ref < bradius)
              que[endindex++] = nid;
          }
        }
      }
    }

    // consider different cases
    if (gates->get(src)) {
      if (gates->get(trg)) {
        // both src and trg are gates
        reachtime++;
        if (labels[src][2] <= labels[trg][2]) {
          comparenum++;
          if (labels[src][0] <= labels[trg][0] &&
              labels[src][1] >= labels[trg][1])
            return true;
        }
        for (outiter = lout[src].begin(); outiter != lout[src].end();
             outiter++) {
          comparenum++;
          if (labels[*outiter][2] > labels[trg][2])
            break;
          if (labels[*outiter][0] <= labels[trg][0] &&
              labels[*outiter][1] >= labels[trg][1])
            return true;
        }
      } else {
        // src is gate and trg is NOT gate
        for (initer = inoutgates[trg][0].begin();
             initer != inoutgates[trg][0].end(); initer++) {
          if (!contains(src, *initer))
            continue;
          reachtime++;
          if (labels[src][2] <= labels[*initer][2]) {
            comparenum++;
            if (labels[src][0] <= labels[*initer][0] &&
                labels[src][1] >= labels[*initer][1])
              return true;
          }
          for (outiter = lout[src].begin(); outiter != lout[src].end();
               outiter++) {
            comparenum++;
            if (labels[*outiter][2] > labels[*initer][2])
              break;
            if (labels[*outiter][0] <= labels[*initer][0] &&
                labels[*outiter][1] >= labels[*initer][1])
              return true;
          }
        }
      }
    } else {
      if (gates->get(trg)) {
        // src is NOT gate and trg is gate
        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          reachtime++;
          if (labels[nid][2] <= labels[trg][2]) {
            comparenum++;
            if (labels[nid][0] <= labels[trg][0] &&
                labels[nid][1] >= labels[trg][1])
              return true;
          }
          for (outiter = lout[nid].begin(); outiter != lout[nid].end();
               outiter++) {
            comparenum++;
            if (labels[*outiter][2] > labels[trg][2])
              break;
            if (labels[*outiter][0] <= labels[trg][0] &&
                labels[*outiter][1] >= labels[trg][1])
              return true;
          }
        }
      } else {
        // both src and trg are NOT gates
        for (int i = 0; i < inoutgates[src][1].size(); i++) {
          nid = inoutgates[src][1][i];
          if (!contains(nid, trg))
            continue;
          for (initer = inoutgates[trg][0].begin();
               initer != inoutgates[trg][0].end(); initer++) {
            reachtime++;
            if (labels[nid][2] <= labels[*initer][2]) {
              comparenum++;
              if (labels[nid][0] <= labels[*initer][0] &&
                  labels[nid][1] >= labels[*initer][1])
                return true;
            }
            for (outiter = lout[nid].begin(); outiter != lout[nid].end();
                 outiter++) {
              comparenum++;
              if (labels[*outiter][2] > labels[*initer][2])
                break;
              if (labels[*outiter][0] <= labels[*initer][0] &&
                  labels[*outiter][1] >= labels[*initer][1])
                return true;
            }
          }
        }
      }
    }

    return false;
  }
};


} // namespace lotus::cfl::cs_index::flare::path_tree

#endif
