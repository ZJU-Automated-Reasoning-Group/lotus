#pragma once

#include <string>

namespace mpi {

enum class NormalizationConfidence {
  ExactMPI,
  PMPIWrapper,
  KnownOpenMPIForwarder,
  UnknownVendorInternal
};

inline const char *toString(NormalizationConfidence confidence) {
  switch (confidence) {
  case NormalizationConfidence::ExactMPI:
    return "exact_mpi";
  case NormalizationConfidence::PMPIWrapper:
    return "pmpi_wrapper";
  case NormalizationConfidence::KnownOpenMPIForwarder:
    return "known_openmpi_forwarder";
  case NormalizationConfidence::UnknownVendorInternal:
    return "unknown_vendor_internal";
  }
  return "unknown_vendor_internal";
}

struct MPISymbolNormalization {
  std::string canonical_name;
  NormalizationConfidence confidence =
      NormalizationConfidence::UnknownVendorInternal;
};

} // namespace mpi
