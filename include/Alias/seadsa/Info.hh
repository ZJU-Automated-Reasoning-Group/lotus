#ifndef __DSA_INFO_HH_
#define __DSA_INFO_HH_

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include <map>
#include <set>
#include <unordered_map>

/* Precompute queries for dsa clients */

namespace llvm {
class DataLayout;
class TargetLibraryInfo;
class TargetLibraryInfoWrapperPass;
class Value;
class Function;
class raw_ostream;
} // namespace llvm

namespace seadsa {
class Node;
class Graph;
class GlobalAnalysis;
} // namespace seadsa

namespace seadsa {

// Wrapper to extend a dsa node with extra information
class NodeInfo {
  const Node *m_node;
  // This id can be either m_node->getId() or some other unique
  // identifier chosen in a different way.
  unsigned m_id;
  unsigned m_accesses;
  // Name of one of the node's referrers.
  // The node is chosen deterministically
  std::string m_rep_name;

public:
  NodeInfo(const Node *node, unsigned id, std::string name)
      : m_node(node), m_id(id), m_accesses(0), m_rep_name(name) {}

  bool operator==(const NodeInfo &o) const {
    // XXX: we do not want to use pointer addresses here
    return m_id == o.m_id;
  }

  bool operator<(const NodeInfo &o) const {
    // XXX: we do not want to use pointer addresses here
    return m_id < o.m_id;
  }

  const Node *getNode() const { return m_node; }
  unsigned getId() const { return m_id; }
  NodeInfo &operator++() { // prefix ++
    m_accesses++;
    return *this;
  }

  unsigned getAccesses() const { return m_accesses; }
};

class DsaInfo {

  typedef std::unordered_map<const Node *, NodeInfo> NodeInfoMap;
  // Replacement for boost::bimap - use two maps
  typedef std::map<const llvm::Value *, unsigned int> AllocSiteToIdMap;
  typedef std::map<unsigned int, const llvm::Value *> IdToAllocSiteMap;
  typedef std::set<const llvm::Value *> ValueSet;
  typedef std::set<unsigned int> IdSet;
  typedef std::set<NodeInfo> NodeInfoSet;
  typedef std::set<Graph *> GraphSet;
  typedef std::unordered_map<const llvm::Value *, std::string> NamingMap;

  const llvm::DataLayout &m_dl;
  llvm::TargetLibraryInfoWrapperPass &m_tliWrapper;
  GlobalAnalysis &m_dsa;
  NodeInfoMap m_nodes_map;            // map Node to NodeInfo
  AllocSiteToIdMap m_alloc_site_to_id; // map allocation sites to id
  IdToAllocSiteMap m_id_to_alloc_site; // reverse map id to allocation sites
  IdSet m_alloc_sites_set;
  NamingMap m_names; // map Value to string name
  GraphSet m_seen_graphs;

  // Custom transform iterator
  template <typename MapIterator>
  class transform_second_iterator {
    MapIterator m_iter;
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = NodeInfo;
    using difference_type = std::ptrdiff_t;
    using pointer = const NodeInfo*;
    using reference = const NodeInfo&;
    
    transform_second_iterator() = default;
    explicit transform_second_iterator(MapIterator iter) : m_iter(iter) {}
    
    reference operator*() const { return m_iter->second; }
    pointer operator->() const { return &m_iter->second; }
    
    transform_second_iterator& operator++() { ++m_iter; return *this; }
    transform_second_iterator operator++(int) { auto tmp = *this; ++m_iter; return tmp; }
    
    bool operator==(const transform_second_iterator& other) const { return m_iter == other.m_iter; }
    bool operator!=(const transform_second_iterator& other) const { return m_iter != other.m_iter; }
  };

  typedef transform_second_iterator<typename NodeInfoMap::const_iterator> nodes_const_iterator;
  typedef llvm::iterator_range<nodes_const_iterator> nodes_const_range;

  nodes_const_iterator nodes_begin() const {
    return nodes_const_iterator(m_nodes_map.begin());
  }

  nodes_const_iterator nodes_end() const {
    return nodes_const_iterator(m_nodes_map.end());
  }

  nodes_const_range nodes() const {
    return llvm::make_range(nodes_begin(), nodes_end());
  }

  struct is_alive_node {
    bool operator()(const NodeInfo &);
  };
  
  // Custom filter iterator
  template <typename Iterator>
  class filter_alive_iterator {
    Iterator m_iter;
    Iterator m_end;
    is_alive_node m_pred;
    
    void skip_to_next() {
      while (m_iter != m_end && !m_pred(*m_iter))
        ++m_iter;
    }
    
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = NodeInfo;
    using difference_type = std::ptrdiff_t;
    using pointer = const NodeInfo*;
    using reference = const NodeInfo&;
    
    filter_alive_iterator() = default;
    filter_alive_iterator(Iterator iter, Iterator end) : m_iter(iter), m_end(end) {
      skip_to_next();
    }
    
    reference operator*() const { return *m_iter; }
    pointer operator->() const { return m_iter.operator->(); }
    
    filter_alive_iterator& operator++() { ++m_iter; skip_to_next(); return *this; }
    filter_alive_iterator operator++(int) { auto tmp = *this; ++(*this); return tmp; }
    
    bool operator==(const filter_alive_iterator& other) const { return m_iter == other.m_iter; }
    bool operator!=(const filter_alive_iterator& other) const { return m_iter != other.m_iter; }
  };
  
  typedef filter_alive_iterator<nodes_const_iterator> live_nodes_const_iterator;

public:
  typedef llvm::iterator_range<live_nodes_const_iterator> live_nodes_const_range;
  typedef IdSet alloc_sites_set;

private:
  live_nodes_const_iterator live_nodes_begin() const {
    return live_nodes_const_iterator(nodes_begin(), nodes_end());
  }

  live_nodes_const_iterator live_nodes_end() const {
    return live_nodes_const_iterator(nodes_end(), nodes_end());
  }

  std::string getName(const llvm::Function &fn, const llvm::Value &v);

  void recordMemAccess(const llvm::Value *v, Graph &g,
                       const llvm::Instruction &I);

  void recordMemAccesses(const llvm::Function &f);

  void assignNodeId(const llvm::Function &fn, Graph *g);

  bool recordAllocSite(const llvm::Value *v, unsigned &site_id);

  void assignAllocSiteId();

public:
  DsaInfo(const llvm::DataLayout &dl,
          llvm::TargetLibraryInfoWrapperPass &tliWrapper, GlobalAnalysis &dsa,
          bool verbose = true)
      : m_dl(dl), m_tliWrapper(tliWrapper), m_dsa(dsa) {}

  bool runOnModule(llvm::Module &M);
  bool runOnFunction(llvm::Function &fn);

  // Iterate over all non-trival Dsa nodes
  live_nodes_const_range live_nodes() const {
    return llvm::make_range(live_nodes_begin(), live_nodes_end());
  }

  // Return the set of all allocation sites
  const alloc_sites_set &alloc_sites() const { return m_alloc_sites_set; }

  ////////
  /// API for Dsa clients
  ////////

  Graph *getDsaGraph(const llvm::Function &f) const;

  bool isAccessed(const Node &n) const;

  // return unique numeric identifier for node n if found,
  // otherwise 0
  unsigned int getDsaNodeId(const Node &n) const;

  // return unique numeric identifier for Value if it is an
  // allocation site, otherwise 0.
  unsigned int getAllocSiteId(const llvm::Value *V) const;

  // the inverse of getAllocSiteID
  const llvm::Value *getAllocValue(unsigned int alloc_site_id) const;
};

class DsaInfoPass : public llvm::ModulePass {
  std::unique_ptr<DsaInfo> m_dsa_info;

public:
  static char ID;

  DsaInfoPass() : ModulePass(ID), m_dsa_info(nullptr) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

  bool runOnModule(llvm::Module &M) override;

  llvm::StringRef getPassName() const override {
    return "Extract stats from SeaDsa analysis";
  }

  DsaInfo &getDsaInfo();
};

} // namespace seadsa
#endif
