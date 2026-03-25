//===- AddressValue.h ----Address Value Sets-------------------------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>

namespace lotus {
namespace analysis {

#define AddressMask 0x7f000000
#define FlippedAddressMask (AddressMask ^ 0xffffffff)
// the address of InvalidMem(the black hole), getVirtualMemAddress(2);
#define InvalidMemAddr 0x7f000000 + 2
// the address of NullMem, getVirtualMemAddress(0);
#define NullMemAddr 0x7f000000

/// AddressValue - represents a set of memory addresses
class AddressValue {
public:
  typedef std::unordered_set<uint32_t> AddrSet;

private:
  AddrSet _addrs;

public:
  static inline uint32_t getInternalID(uint32_t idx) {
    return (idx & FlippedAddressMask);
  }
  AddressValue() {}
  explicit AddressValue(const AddrSet &addrs) : _addrs(addrs) {}
  explicit AddressValue(uint32_t addr) : _addrs({addr}) {}
  ~AddressValue() = default;
  AddressValue(const AddressValue &other) : _addrs(other._addrs) {}
  AddressValue(AddressValue &&other) noexcept
      : _addrs(std::move(other._addrs)) {}

  AddressValue &operator=(const AddressValue &other) {
    if (!this->equals(other)) {
      _addrs = other._addrs;
    }
    return *this;
  }

  AddressValue &operator=(AddressValue &&other) noexcept {
    if (this != &other) {
      _addrs = std::move(other._addrs);
    }
    return *this;
  }

  bool equals(const AddressValue &rhs) const { return _addrs == rhs._addrs; }

  AddrSet::const_iterator begin() const { return _addrs.cbegin(); }
  AddrSet::const_iterator end() const { return _addrs.cend(); }

  bool empty() const { return _addrs.empty(); }
  size_t size() const { return _addrs.size(); }

  std::pair<AddrSet::iterator, bool> insert(uint32_t id) {
    return _addrs.insert(id);
  }

  const AddrSet &getVals() const { return _addrs; }
  void setVals(const AddrSet &vals) { _addrs = vals; }

  bool join_with(const AddressValue &other) {
    bool changed = false;
    for (const auto &addr : other) {
      if (!_addrs.count(addr)) {
        if (insert(addr).second)
          changed = true;
      }
    }
    return changed;
  }

  bool meet_with(const AddressValue &other) {
    AddrSet s;
    for (const auto &id : other._addrs) {
      if (_addrs.find(id) != _addrs.end()) {
        s.insert(id);
      }
    }
    bool changed = (_addrs != s);
    _addrs = std::move(s);
    return changed;
  }

  bool contains(uint32_t id) const { return _addrs.count(id); }

  bool hasIntersect(const AddressValue &other) {
    AddressValue v = *this;
    v.meet_with(other);
    return !v.empty();
  }

  bool isBottom() const { return empty(); }

  std::string toString() const {
    std::stringstream rawStr;
    if (isBottom()) {
      rawStr << "⊥";
    } else {
      rawStr << "[";
      for (auto it = _addrs.begin(), eit = _addrs.end(); it != eit; ++it) {
        rawStr << *it;
        if (std::next(it) != eit)
          rawStr << ", ";
      }
      rawStr << "]";
    }
    return rawStr.str();
  }

  static inline uint32_t getVirtualMemAddress(uint32_t idx) {
    assert(idx != 0 && "idx can't be 0 because it represents a nullptr");
    return AddressMask + idx;
  }

  static inline bool isVirtualMemAddress(uint32_t val) {
    return (val & 0xff000000) == AddressMask;
  }

  static inline uint32_t getIDFromAddr(uint32_t addr) {
    return getInternalID(addr);
  }
};

} // namespace analysis
} // namespace lotus
