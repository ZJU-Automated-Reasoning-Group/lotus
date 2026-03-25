//===-- Verification/Sifa/Storage/ILoiStorage.h ---------------------------===//
//
// Storage for predicates/states at "locations of interest" (ported from Sifa).
//
// In Ultimate, LOIs are IcfgLocations and stored predicates are IPredicates.
// In lotus we store generic states keyed by an LLVM BasicBlock (nullable for
// "anonymous" markers, e.g. in StarDagCache).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_STORAGE_ILOISTORAGE_H
#define LOTUS_VERIFICATION_SIFA_STORAGE_ILOISTORAGE_H

namespace lotus {
namespace sifa {

template <typename LocationT, typename StateT> class ILoiStorage {
public:
  virtual ~ILoiStorage() = default;
  virtual void store(LocationT location, const StateT &state) = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_STORAGE_ILOISTORAGE_H
