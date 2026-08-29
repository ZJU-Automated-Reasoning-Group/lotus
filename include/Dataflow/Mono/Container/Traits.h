#ifndef LOTUS_DATAFLOW_MONO_CONTAINER_TRAITS_H_
#define LOTUS_DATAFLOW_MONO_CONTAINER_TRAITS_H_

#include "Dataflow/Mono/Container/BitVectorSet.h"

#include <set>
#include <type_traits>

namespace mono {

/**
 * @brief Traits for fact container types used in monotone dataflow analysis
 *
 * This provides a unified interface for different container implementations
 * (std::set, BitVectorSet, etc.) so analyses can choose the best container
 * for their needs without hardcoding.
 *
 * **Required operations:**
 * - insert(element) -> bool
 * - erase(element) -> bool
 * - contains(element) -> bool
 * - empty() -> bool
 * - size() -> size_t
 * - begin()/end() for iteration
 * - operator== for equality
 *
 * **Optional operations (for performance):**
 * - unionWith(other) for efficient union
 * - intersectWith(other) for efficient intersection
 * - differenceWith(other) for efficient difference
 */

/**
 * @brief Container type selector for analyses
 *
 * Use this to choose between different container implementations:
 *
 * ```cpp
 * // Use std::set (default, good for small universes)
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<SetContainer<Value*>>;
 *
 * // Use BitVectorSet (good for large universes)
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<BitVectorContainer<Value*>>;
 *
 * // Use custom container
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<MyCustomContainer<Value*>>;
 * ```
 */

// ============================================================================
// Standard std::set wrapper (default choice)
// ============================================================================

/**
 * @brief Standard set-based container (std::set wrapper)
 *
 * **Best for:**
 * - Small universes (<100 elements)
 * - Sparse fact sets
 * - When universe is unknown or dynamic
 *
 * **Performance:**
 * - Insert/Erase: O(log N)
 * - Union/Intersection: O(N log N)
 * - Memory: ~24 bytes per element
 */
template <typename T> class SetContainer {
public:
  using value_type = T;
  using container_type = std::set<T>;

  SetContainer() = default;
  SetContainer(std::initializer_list<T> Init) : Data(Init) {}

  // Copy and move semantics (defaulted, but explicit for clarity)
  SetContainer(const SetContainer &) = default;
  SetContainer(SetContainer &&) = default;
  SetContainer &operator=(const SetContainer &) = default;
  SetContainer &operator=(SetContainer &&) = default;
  ~SetContainer() = default;

  // Iterator types must be declared before methods that use them
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::const_iterator;

  bool insert(const T &Elem) { return Data.insert(Elem).second; }

  bool erase(const T &Elem) { return Data.erase(Elem) > 0; }

  iterator erase(iterator It) { return Data.erase(It); }

  bool contains(const T &Elem) const { return Data.find(Elem) != Data.end(); }

  size_t count(const T &Elem) const { return Data.count(Elem); }

  bool empty() const { return Data.empty(); }

  size_t size() const { return Data.size(); }

  void clear() { Data.clear(); }

  void unionWith(const SetContainer &Other) {
    Data.insert(Other.Data.begin(), Other.Data.end());
  }

  void intersectWith(const SetContainer &Other) {
    SetContainer Result;
    std::set_intersection(Data.begin(), Data.end(), Other.Data.begin(),
                          Other.Data.end(),
                          std::inserter(Result.Data, Result.Data.begin()));
    Data = std::move(Result.Data);
  }

  void differenceWith(const SetContainer &Other) {
    for (const auto &Elem : Other.Data) {
      Data.erase(Elem);
    }
  }

  bool operator==(const SetContainer &Other) const {
    return Data == Other.Data;
  }

  bool operator!=(const SetContainer &Other) const { return !(*this == Other); }

  // Iterator support (already declared above)

  iterator begin() { return Data.begin(); }
  iterator end() { return Data.end(); }
  const_iterator begin() const { return Data.begin(); }
  const_iterator end() const { return Data.end(); }
  const_iterator cbegin() const { return Data.cbegin(); }
  const_iterator cend() const { return Data.cend(); }

  // Access underlying container (for compatibility)
  const container_type &getSet() const { return Data; }
  container_type &getSet() { return Data; }

  // Conversion to std::set for compatibility with legacy APIs
  operator container_type() const { return Data; }
  container_type toStdSet() const { return Data; }

private:
  container_type Data;
};

// ============================================================================
// Bit-vector optimized container
// ============================================================================

/**
 * @brief Bit-vector optimized container (BitVectorSet wrapper)
 *
 * **Best for:**
 * - Large universes (>100 elements)
 * - Dense fact sets
 * - When universe is known in advance
 * - Performance-critical analyses
 *
 * **Performance:**
 * - Insert/Erase: O(1) bit operations
 * - Union/Intersection: O(N/64) bit operations
 * - Memory: ~N/8 bytes total (much less than std::set)
 *
 * **Requirements:**
 * - Universe must be set before use (via setUniverse())
 * - All instances should share the same universe
 */
template <typename T> class BitVectorContainer {
public:
  using value_type = T;
  using container_type = BitVectorSet<T>;

  BitVectorContainer() = default;

  // Copy and move semantics (defaulted, but explicit for clarity)
  BitVectorContainer(const BitVectorContainer &) = default;
  BitVectorContainer(BitVectorContainer &&) = default;
  BitVectorContainer &operator=(const BitVectorContainer &) = default;
  BitVectorContainer &operator=(BitVectorContainer &&) = default;
  ~BitVectorContainer() = default;

  /**
   * @brief Set the universe for this container
   *
   * Must be called before using insert/contains operations.
   * All containers in an analysis should share the same universe.
   */
  void setUniverse(const std::vector<T> &Universe) {
    Data.setUniverse(Universe);
  }

  // Iterator types must be declared before methods that use them
  using iterator = typename container_type::iterator;
  using const_iterator = typename container_type::iterator;

  bool insert(const T &Elem) { return Data.insert(Elem); }

  bool erase(const T &Elem) { return Data.erase(Elem); }

  iterator erase(iterator It) {
    // BitVectorSet doesn't support iterator erase, so we need to find the
    // element and erase by value. This is less efficient but necessary for
    // compatibility.
    if (It != end()) {
      T Elem = *It;
      ++It; // Advance before erasing
      Data.erase(Elem);
      return It;
    }
    return It;
  }

  bool contains(const T &Elem) const { return Data.contains(Elem); }

  size_t count(const T &Elem) const { return Data.contains(Elem) ? 1 : 0; }

  bool empty() const { return Data.empty(); }

  size_t size() const { return Data.size(); }

  void clear() { Data.clear(); }

  void unionWith(const BitVectorContainer &Other) {
    Data.unionWith(Other.Data);
  }

  void intersectWith(const BitVectorContainer &Other) {
    Data.intersectWith(Other.Data);
  }

  void differenceWith(const BitVectorContainer &Other) {
    Data.differenceWith(Other.Data);
  }

  bool operator==(const BitVectorContainer &Other) const {
    return Data == Other.Data;
  }

  bool operator!=(const BitVectorContainer &Other) const {
    return !(*this == Other);
  }

  // Iterator support (already declared above)

  iterator begin() { return Data.begin(); }
  iterator end() { return Data.end(); }
  const_iterator begin() const { return Data.begin(); }
  const_iterator end() const { return Data.end(); }
  const_iterator cbegin() const { return Data.begin(); }
  const_iterator cend() const { return Data.end(); }

  // Access underlying container
  const container_type &getBitVector() const { return Data; }
  container_type &getBitVector() { return Data; }

private:
  container_type Data;
};

// ============================================================================
// Container type detection and helpers
// ============================================================================

/**
 * @brief Detect if a container type supports setUniverse()
 *
 * Used to determine if a container needs universe initialization.
 */
template <typename ContainerT> struct HasSetUniverse {
private:
  template <typename U>
  static auto test(int)
      -> decltype(std::declval<U>().setUniverse(
                      std::declval<std::vector<typename U::value_type>>()),
                  std::true_type{});

  template <typename> static std::false_type test(...);

public:
  static constexpr bool value = decltype(test<ContainerT>(0))::value;
};

/**
 * @brief Helper to initialize container universe if needed
 *
 * This function will set the universe for containers that require it
 * (like BitVectorContainer) and do nothing for others (like SetContainer).
 */
template <typename ContainerT>
typename std::enable_if<HasSetUniverse<ContainerT>::value>::type
initializeContainerUniverse(
    ContainerT &Container,
    const std::vector<typename ContainerT::value_type> &Universe) {
  Container.setUniverse(Universe);
}

template <typename ContainerT>
typename std::enable_if<!HasSetUniverse<ContainerT>::value>::type
initializeContainerUniverse(
    ContainerT &Container,
    const std::vector<typename ContainerT::value_type> &Universe) {
  // No-op for containers that don't need universe initialization
  (void)Container;
  (void)Universe;
}

/**
 * @brief Helper to create an empty container with optional universe
 */
template <typename ContainerT>
ContainerT createEmptyContainer(
    const std::vector<typename ContainerT::value_type> *Universe = nullptr) {
  ContainerT Container;
  if (Universe != nullptr) {
    initializeContainerUniverse(Container, *Universe);
  }
  return Container;
}

// ============================================================================
// Type aliases for common use cases
// ============================================================================

/**
 * @brief Default container type for Value* facts
 *
 * Use this when you don't have a preference:
 * ```cpp
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<DefaultValueContainer>;
 * ```
 */
template <typename T> using DefaultContainer = SetContainer<T>;

/**
 * @brief Fast container type for Value* facts (bit-vector optimized)
 *
 * Use this for performance-critical analyses with large universes:
 * ```cpp
 * using MyAnalysisTypes = LLVMMonoAnalysisTypes<FastValueContainer>;
 * ```
 */
template <typename T> using FastContainer = BitVectorContainer<T>;

// ============================================================================
// Legacy compatibility: direct std::set support
// ============================================================================

/**
 * @brief Adapter to use std::set directly (for backward compatibility)
 *
 * This allows existing code using std::set<T> to work without changes.
 * However, prefer SetContainer<T> for new code as it provides a cleaner
 * interface.
 */
template <typename T> struct StdSetAdapter {
  using value_type = T;
  using container_type = std::set<T>;

  std::set<T> Data;

  bool insert(const T &Elem) { return Data.insert(Elem).second; }
  bool erase(const T &Elem) { return Data.erase(Elem) > 0; }
  bool contains(const T &Elem) const { return Data.find(Elem) != Data.end(); }
  bool empty() const { return Data.empty(); }
  size_t size() const { return Data.size(); }
  void clear() { Data.clear(); }

  void unionWith(const StdSetAdapter &Other) {
    Data.insert(Other.Data.begin(), Other.Data.end());
  }

  void intersectWith(const StdSetAdapter &Other) {
    std::set<T> Result;
    std::set_intersection(Data.begin(), Data.end(), Other.Data.begin(),
                          Other.Data.end(),
                          std::inserter(Result, Result.begin()));
    Data = std::move(Result);
  }

  void differenceWith(const StdSetAdapter &Other) {
    for (const auto &Elem : Other.Data) {
      Data.erase(Elem);
    }
  }

  bool operator==(const StdSetAdapter &Other) const {
    return Data == Other.Data;
  }
  bool operator!=(const StdSetAdapter &Other) const {
    return !(*this == Other);
  }

  using iterator = typename std::set<T>::iterator;
  using const_iterator = typename std::set<T>::const_iterator;
  iterator begin() { return Data.begin(); }
  iterator end() { return Data.end(); }
  const_iterator begin() const { return Data.begin(); }
  const_iterator end() const { return Data.end(); }
};

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_CONTAINER_TRAITS_H_
