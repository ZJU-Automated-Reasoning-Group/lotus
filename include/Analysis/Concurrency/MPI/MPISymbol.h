#pragma once

#include "Analysis/Concurrency/MPI/MPINormalization.h"

#include <cctype>
#include <string>

#include <llvm/ADT/StringRef.h>

namespace mpi {

inline MPISymbolNormalization normalizeMPISymbol(llvm::StringRef raw_name) {
  llvm::StringRef name = raw_name;
  while (name.startswith("\01")) {
    name = name.drop_front();
  }

  if (name.startswith("_") && !name.startswith("__wrap_")) {
    name = name.drop_front();
  }

  MPISymbolNormalization result;
  result.canonical_name = name.str();
  result.confidence = NormalizationConfidence::UnknownVendorInternal;

  if (name.startswith("__wrap_")) {
    llvm::StringRef wrapped = name.drop_front(7);
    if (wrapped.startswith("MPI_")) {
      result.canonical_name = wrapped.str();
      result.confidence = NormalizationConfidence::PMPIWrapper;
      return result;
    }
    if (wrapped.startswith("PMPI_")) {
      result.canonical_name =
          (std::string("MPI_") + wrapped.drop_front(5).str());
      result.confidence = NormalizationConfidence::PMPIWrapper;
      return result;
    }
    result.canonical_name = wrapped.str();
    result.confidence = NormalizationConfidence::UnknownVendorInternal;
    return result;
  }

  if (name.startswith("PMPI_")) {
    result.canonical_name = std::string("MPI_") + name.drop_front(5).str();
    result.confidence = NormalizationConfidence::PMPIWrapper;
    return result;
  }

  if (name.startswith("ompi_mpi_")) {
    result.canonical_name = std::string("MPI_") + name.drop_front(9).str();
    result.confidence = NormalizationConfidence::KnownOpenMPIForwarder;
    return result;
  }

  if (name.startswith("pmpi_")) {
    result.canonical_name = std::string("MPI_") + name.drop_front(5).str();
    result.confidence = NormalizationConfidence::PMPIWrapper;
    return result;
  }

  if (name.startswith("mpi_")) {
    result.canonical_name = std::string("MPI_") + name.drop_front(4).str();
    result.confidence = NormalizationConfidence::ExactMPI;
    return result;
  }

  if (name.startswith("MPI_")) {
    result.canonical_name = name.str();
    result.confidence = NormalizationConfidence::ExactMPI;
    return result;
  }

  if (name.startswith("ompi_")) {
    result.confidence = NormalizationConfidence::UnknownVendorInternal;
    return result;
  }

  result.confidence = NormalizationConfidence::ExactMPI;
  return result;
}

inline std::string normalizeMPISymbolName(llvm::StringRef raw_name) {
  return normalizeMPISymbol(raw_name).canonical_name;
}

inline bool equalsCaseInsensitiveASCII(llvm::StringRef lhs,
                                       llvm::StringRef rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    unsigned char lc = static_cast<unsigned char>(lhs[i]);
    unsigned char rc = static_cast<unsigned char>(rhs[i]);
    if (std::tolower(lc) != std::tolower(rc)) {
      return false;
    }
  }
  return true;
}

} // namespace mpi
