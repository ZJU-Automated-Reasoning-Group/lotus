/* Anatree : A Data Structure for Anagrams.
 *
 * Copyright (C) 2023  Steffan Christ Sølvsten
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Lesse General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ANATREE_H
#define ANATREE_H

// 'assert(...)'
#include <assert.h>

// 'std::sort(...)'
#include <algorithm>

// 'std::unique_ptr' and 'std::stringstream' for 'anatree<>::node'
#include <memory>
#include <sstream>

// 'std::string', 'std::unordered_set', and 'std::unordered_map' for
// 'anatree<...>' template parameters
#include <iterator>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

////////////////////////////////////////////////////////////////////////////////
/// \mainpage
/// # Anagram Tree
///
/// The *anagram tree* (\ref anatree) is a data structure specifically designed
/// to efficiently store a dictionary of words such that (sub)anagrams can be
/// identified quickly. See \ref anatree for all operations supported.
///
/// To achieve its efficiency, it makes use of a trie-like representation that
/// has properties quite similar to Binary Decision Diagrams (BDDs). See the
/// [paper](https://dl.acm.org/doi/abs/10.1145/2133803.2133804) by Charles Brown
/// or the following
/// [slides](https://raw.githubusercontent.com/SSoelvsten/slides/out/anatree.pdf)
/// by Steffan Sølvsten for more details.
///
/// \author Steffan Sølvsten and Andreas H. H. Hansen
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// \brief A data structure capable of storing a set of 'std::string' (or
///        similar) data structures, enabling quick access to all 'anagrams' of
///        each word (within or not).
///
/// \tparam T       Type for words, i.e. lists of elemnts. This type should
///                 provide the interface of `std::string`, such as
///                 `std::basic_string<_>` and `std::vector<_>`.
///
/// \tparam Compare Ordering of the symbols within each word. The order has a
///                 major impact on the size.
///
/// \tparam Set     Type to be used for storing and returning sets of words.
///
/// \tparam Map     Type to be used for storing pairs `T`.
////////////////////////////////////////////////////////////////////////////////
template <typename T = std::string,
          typename Compare = std::less<typename T::value_type>,
          typename Set = std::unordered_set<T>,
          typename Map = std::unordered_map<T, T>>
class anatree {
private:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Type of each individual character.
  //////////////////////////////////////////////////////////////////////////////
  using value_type = typename T::value_type;

private:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Individual node of the Anatree.
  //////////////////////////////////////////////////////////////////////////////
  class node {
  public:
    using ptr = std::unique_ptr<node>;

    // TODO: derive a non-useful value as 'NIL'.
    static constexpr value_type NIL = 0;

  public:
    ////////////////////////////////////////////////////////////////////////////
    /// \brief Character in this node
    ////////////////////////////////////////////////////////////////////////////
    value_type m_char = NIL;

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Binary choice on children.
    ///
    /// Follow the 'false' pointer, if 'm_char' does not occur in the word.
    /// Otherwise follow the 'true' pointer, if it does.
    ////////////////////////////////////////////////////////////////////////////
    ptr m_children[2] = {nullptr, nullptr};

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Set of words that are anagrams of the path up to this node.
    ////////////////////////////////////////////////////////////////////////////
    Set m_words;

  public:
    ////////////////////////////////////////////////////////////////////////////
    /// \brief Initialize a NIL node
    ////////////////////////////////////////////////////////////////////////////
    void init(value_type c, ptr f_ptr = nullptr, ptr t_ptr = nullptr) {
      assert(m_char == NIL && c != NIL);
      m_char = c;
      m_children[false] = f_ptr ? std::move(f_ptr) : make_node();
      m_children[true] = t_ptr ? std::move(t_ptr) : make_node();
    }

  public:
    ////////////////////////////////////////////////////////////////////////////
    /// \brief Empty (NIL) constructor
    ////////////////////////////////////////////////////////////////////////////
    node() = default;

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Empty (NIL) ptr constructor
    ////////////////////////////////////////////////////////////////////////////
    static ptr make_node() { return std::make_unique<node>(); }

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Copy constructor
    ////////////////////////////////////////////////////////////////////////////
    node(const node &) = default;

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Non-nil ptr constructor
    ////////////////////////////////////////////////////////////////////////////
    node(const value_type c, ptr f_ptr = make_node(), ptr t_ptr = make_node()) {
      init(c, std::move(f_ptr), std::move(t_ptr));
    };

    ////////////////////////////////////////////////////////////////////////////
    /// \brief Non-nil constructor
    ////////////////////////////////////////////////////////////////////////////
    static ptr make_node(value_type c, ptr f_ptr = make_node(),
                         ptr t_ptr = make_node()) {
      return std::make_unique<node>(c, std::move(f_ptr), std::move(t_ptr));
    }

  public:
    ////////////////////////////////////////////////////////////////////////////
    std::string to_string() {
      std::stringstream ss;
      ss << "{ char: " << m_char << ", children: { " << m_children[false]
         << ", " << m_children[true] << " } }";
      return ss.str();
    }
  };

private:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Alias for 'node::ptr' that allows omitting the 'typename' keyword.
  //////////////////////////////////////////////////////////////////////////////
  using node_ptr = typename node::ptr;

private:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Comparator for characters.
  //////////////////////////////////////////////////////////////////////////////
  Compare m_char_comp = Compare();

  //////////////////////////////////////////////////////////////////////////////
  /// \brief Root of the anatree (initially a NIL node).
  //////////////////////////////////////////////////////////////////////////////
  node_ptr m_root = node::make_node();

  //////////////////////////////////////////////////////////////////////////////
  /// \brief Number of words stored within this tree.
  //////////////////////////////////////////////////////////////////////////////
  size_t m_size = 0u;

  //////////////////////////////////////////////////////////////////////////////
  /// \brief Number of nodes (including NIL) in the tree.
  //////////////////////////////////////////////////////////////////////////////
  size_t m_tree_size = 1u;

  //////////////////////////////////////////////////////////////////////////////
  /// \brief Unique `null` pointer to have a *no* return value.
  //////////////////////////////////////////////////////////////////////////////
  const node_ptr m_null = nullptr;

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Constructor of an empty Anatree.
  //////////////////////////////////////////////////////////////////////////////
  anatree(Compare char_comp = Compare()) : m_char_comp(char_comp) {}

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Copy-constructor, creating a deep copy of another Anatree.
  ///
  /// \warning Requires \f$O(N)\f$ time.
  //////////////////////////////////////////////////////////////////////////////
  anatree(const anatree &a)
      : m_char_comp(a.m_char_comp), m_root(deep_copy(a.m_root)),
        m_size(a.m_size), m_tree_size(a.m_tree_size) {}

private:
  static node_ptr deep_copy(const node_ptr &p) {
    node_ptr np = node::make_node();

    // Case: NIL
    // -> Leave node as-is
    if (p->m_char == node::NIL) {
      assert(p->m_children[false] == nullptr && p->m_children[true] == nullptr);
    }

    // Case: not NIL
    // -> Obtain recursively deep copy of children and copy over 'm_char'
    else {
      np->m_char = p->m_char;
      np->m_children[false] = deep_copy(p->m_children[false]);
      np->m_children[true] = deep_copy(p->m_children[true]);
    }

    // Copy over the set of words stored within this node.
    np->m_words = p->m_words;
    return std::move(np);
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Move-constructor, taking over ownership of the other's tree.
  //////////////////////////////////////////////////////////////////////////////
  anatree(anatree &&) = default;

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Contruct a tree containing all of the given words.
  //////////////////////////////////////////////////////////////////////////////
  template <typename InputIt> anatree(InputIt first, InputIt last) : anatree() {
    using ValueType = typename std::iterator_traits<InputIt>::value_type;
    static_assert(std::is_convertible<ValueType, T>::value,
                  "Input iterator value type must be convertible to T");
    insert(first, last);
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Adds the word 'w' to the anatree.
  //////////////////////////////////////////////////////////////////////////////
  void insert(const T &w) {
    T key = sorted_word(w);
    m_root = insert__rec(std::move(m_root), w, key.begin(), key.end());
  }

private:
  node_ptr insert__rec(node_ptr &&p, const T &w, typename T::iterator curr,
                       const typename T::iterator end) {
    assert(p != nullptr);

    // Case: Iterator done
    // -> Insert word
    if (curr == end) {
      if (p->m_words.find(w) == p->m_words.end()) {
        m_size++;
        p->m_words.insert(w);
      }
      return p;
    }

    // Case: NIL
    // -> Turn into non-NIL node
    if (p->m_char == node::NIL) {
      assert(p->m_children[false] == nullptr && p->m_children[true] == nullptr);
      p->init(*curr);
      m_tree_size += 2; // <- NIL 'false' and 'true' children
      p->m_children[true] =
          insert__rec(std::move(p->m_children[true]), w, ++curr, end);
      return p;
    }

    // Case: Iterator behind
    // -> Insert new node in-between
    if (m_char_comp(*curr, p->m_char)) {
      // Hand over ownership of 'p' to a new node 'np'. The original node 'p' is
      // now available as its 'false' child.
      node_ptr np = node::make_node(*curr, std::move(p), node::make_node());
      np->m_words = std::move(np->m_children[false]->m_words);
      np->m_children[false]->m_words = Set();
      m_tree_size += 2; // <- new node and its NIL 'false' child
      np->m_children[true] =
          insert__rec(std::move(np->m_children[true]), w, ++curr, end);
      return std::move(np);
    }

    // Case: Iterator ahead
    // -> Follow 'false' child
    if (m_char_comp(p->m_char, *curr)) {
      p->m_children[false] =
          insert__rec(std::move(p->m_children[false]), w, curr, end);
      return p;
    }

    // Case: Iterator and node matches
    // -> Follow 'true' child
    p->m_children[true] =
        insert__rec(std::move(p->m_children[true]), w, ++curr, end);
    return p;
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Adds the words to the anatree as per the iterator.
  //////////////////////////////////////////////////////////////////////////////
  template <typename InputIt> void insert(InputIt begin, InputIt end) {
    using ValueType = typename std::iterator_traits<InputIt>::value_type;
    static_assert(std::is_convertible<ValueType, T>::value,
                  "Input iterator value type must be convertible to T");
    while (begin != end) {
      insert(*(begin++));
    }
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Remove all nodes/anagrams.
  //////////////////////////////////////////////////////////////////////////////
  void clear() {
    m_root = node::make_node();
    m_size = 0u;
    m_tree_size = 1u;
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Obtain all words in the Anatree, excluding words that are a
  ///        subanagram of another one returned.
  //////////////////////////////////////////////////////////////////////////////
  Set keys() const {
    const auto rec_result = keys__rec(m_root);
    Set ret;
    for (const auto &kv : rec_result) {
      ret.insert(kv.second);
    }
    return ret;
  }

private:
  Map keys__rec(const node_ptr &p) const {
    // Case: Leaf of Tree
    // -> Add a word, if any.
    if (p->m_char == node::NIL) {
      Map ret;
      if (p->m_words.size() > 0) {
        ret[{}] = *p->m_words.begin();
      }
      return ret;
    }

    // Case: Internal Node
    // -> Recurse for words with and without this character
    auto rec_true = keys__rec(p->m_children[true]);
    auto rec_false = keys__rec(p->m_children[false]);

    Map ret;

    // -> Copy over words excluding current node's character (except for ones
    //    that are superseeded)
    for (const auto &rec_false_entry : rec_false) {
      const T &rec_false_k = rec_false_entry.first;
      const T &rec_false_v = rec_false_entry.second;
      // Check whether the word is superseeded by any word in 'rec_true'.
      // - (a) Simple O(1) case
      if (rec_true.find(rec_false_v) != rec_true.end()) {
        continue;
      }

      // - (b) Expensive O(T) case
      bool superseeded = false;
      for (const auto &rec_true_entry : rec_true) {
        const T &rec_true_k = rec_true_entry.first;
        int false_idx = rec_false_k.length() - 1;
        int true_idx = rec_true_k.length() - 1;

        while (0 <= false_idx && 0 <= true_idx) {
          const value_type false_char = rec_false_k[false_idx];
          const value_type true_char = rec_true_k[true_idx];

          if (m_char_comp(true_char, false_char)) {
            true_idx--;
            continue;
          } else if (false_char == true_char) {
            false_idx--;
            true_idx--;
            continue;
          } else { // False includes character not in True path.
            break;
          }
        }
        if (false_idx < 0 && true_idx < 0) {
          superseeded = true;
          break;
        }
      }
      if (superseeded) {
        continue;
      }

      // Add word
      T curr_key(rec_false_k);
      curr_key.push_back(p->m_char);

      assert(ret.find(curr_key) == ret.end());
      ret[curr_key] = rec_false_v;
    }
    rec_false.clear(); // <-- Save on memory

    // -> Copy over words including current node's character.
    for (const auto &rec_entry : rec_true) {
      T curr_key(rec_entry.first);
      curr_key.push_back(p->m_char);

      ret[curr_key] = rec_entry.second;
    }
    // rec_true.clear(); // <-- Save on memory (unecessary)

    return ret;
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Obtain all words in the Anatree of a certain length, excluding
  ///        words that are an anagram of another one returned.
  ///
  /// \param word_length The length of the desired words.
  //////////////////////////////////////////////////////////////////////////////
  Set keys(const size_t word_length) const {
    Set res;
    keys__rec(word_length, m_root, 0, res);
    return res;
  }

private:
  void keys__rec(const size_t word_length, const node_ptr &p,
                 const size_t true_edges, Set &res) const {
    assert(true_edges <= word_length);

    // Case: Found word of 'word_length'
    // -> Search succesful (no need to keep on searching deeper)
    if (word_length == true_edges) {
      if (p->m_words.size() > 0) {
        res.insert(*p->m_words.begin());
      }
      return;
    }

    // Case: Tree stopped early
    // -> Abandon subtree
    if (p->m_char == node::NIL) {
      return;
    }

    // Case: Missing characters
    // -> Merge recursively from false and true subtrees
    keys__rec(word_length, p->m_children[true], true_edges + 1, res);
    keys__rec(word_length, p->m_children[false], true_edges, res);
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Whether there exists an anagrams of 'w'.
  ///
  /// \details An anagram is a word that can be created from (all) the letters
  ///          of 'w'.
  //////////////////////////////////////////////////////////////////////////////
  bool has_anagram_of(const T &w) const {
    const node_ptr &n = find_node(w);
    return n && n->m_words.size() > 0;
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Obtain all words that are anagrams of 'w'.
  ///
  /// \details An anagram is a word that can be created from (all) the letters
  ///          of 'w'.
  //////////////////////////////////////////////////////////////////////////////
  Set anagrams_of(const T &w) const {
    const node_ptr &n = find_node(w);
    return n ? n->m_words : Set();
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Obtain all words that are subanagrams of 'w'.
  ///
  /// \details A subanagram is a word that can be created from some (but not
  ///          necessarily all) letters of 'w'.
  //////////////////////////////////////////////////////////////////////////////
  Set subanagrams_of(const T &w) const {
    T key = sorted_word(w);
    Set res;
    subanagrams_of__rec(m_root, key.begin(), key.end(), res);
    return res;
  }

private:
  void subanagrams_of__rec(const node_ptr &p, typename T::iterator curr,
                           const typename T::iterator end, Set &res) const {
    res.insert(p->m_words.begin(), p->m_words.end());

    // Case: Anatree is done
    // -> Stop
    if (p->m_char == node::NIL) {
      return;
    }

    // Case: Iterator behind
    // -> Skip missing characters
    while (curr != end && m_char_comp(*curr, p->m_char)) {
      ++curr;
    }

    // Case: Iterator done
    // -> Stop
    if (curr == end) {
      return;
    }

    // Case: Iterator ahead
    // -> Follow 'false' child
    if (m_char_comp(p->m_char, *curr)) {
      subanagrams_of__rec(p->m_children[false], curr, end, res);
      return;
    }

    // Case: Iterator and node matches
    // -> Follow both children, merge results and add words on current node
    ++curr;
    subanagrams_of__rec(p->m_children[false], curr, end, res);
    subanagrams_of__rec(p->m_children[true], curr, end, res);
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Whether the Anatree includes the word 'w'.
  //////////////////////////////////////////////////////////////////////////////
  bool contains(const T &w) const {
    const node_ptr &n = find_node(w);
    return n && n->m_words.find(w) != n->m_words.end();
  }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Number of stored words.
  //////////////////////////////////////////////////////////////////////////////
  size_t size() const { return m_size; }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Whether the Anatree is empty.
  //////////////////////////////////////////////////////////////////////////////
  bool empty() const { return size() == 0u; }

public:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Number of nodes within the tree.
  //////////////////////////////////////////////////////////////////////////////
  size_t tree_size() const { return m_tree_size; }

private:
  //////////////////////////////////////////////////////////////////////////////
  /// \brief Creates a copy of the word 'w' with its characters sorted.
  //////////////////////////////////////////////////////////////////////////////
  T sorted_word(const T &w) const {
    T ret(w);
    std::sort(ret.begin(), ret.end(), m_char_comp);
    return ret;
  }

  //////////////////////////////////////////////////////////////////////////////
  /// \brief Traverses the anatree in search of the node corresponding to `w`.
  ///
  /// \returns Immutable reference to the node in the tree. If there is none,
  ///          then it returns `m_null`.
  /////////////////////////////////////////////////////////////////////////////
  const node_ptr &find_node(const T &w) const {
    T key = sorted_word(w);
    return find_node__rec(m_root, key.begin(), key.end());
  }

  const node_ptr &find_node__rec(const node_ptr &p, typename T::iterator curr,
                                 const typename T::iterator end) const {
    // Case: Iterator is done
    if (curr == end) {
      return p;
    }

    // Case: Iterator behind or tree is done
    // -> No words with all letters exist, return Ø .
    if (m_char_comp(*curr, p->m_char) || p->m_char == node::NIL) {
      return m_null;
    }

    // Case: Iterator ahead
    // -> Follow 'false' child
    if (m_char_comp(p->m_char, *curr)) {
      return find_node__rec(p->m_children[false], curr, end);
    }

    // Case: Iterator and node matches
    // -> Follow 'true' child
    return find_node__rec(p->m_children[true], ++curr, end);
  }
};

#endif // ANATREE_H