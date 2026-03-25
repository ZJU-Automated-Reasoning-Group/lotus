# Structured concurrency runtime metadata.
# Format:
#   SymbolOrPrefix TD_TYPE library=<name> semantic=<tag> [traits=<...>] [match=exact|prefix]
#
# Note: OpenMP and MPI APIs have been moved to separate files:
#   - config/openmp_api.spec
#   - config/mpi_api.spec
#
# This file now contains only C++ and other miscellaneous concurrency APIs.

# POSIX semaphores default to capacity-bearing semantics.
sem_wait TD_ACQUIRE library=custom semantic=sem-wait traits=semaphore
sem_post TD_RELEASE library=custom semantic=sem-post traits=semaphore

# Example binary semaphore wrappers can opt into exclusion semantics explicitly.
binary_sem_wait TD_ACQUIRE library=custom semantic=binary-sem-wait traits=semaphore,binary-semaphore
binary_sem_post TD_RELEASE library=custom semantic=binary-sem-post traits=semaphore,binary-semaphore

# C++20 Semaphores
__pthread_sem_init TD_BAR_INIT library=cpp semantic=sem-init
__pthread_sem_wait TD_SEMAPHORE_ACQUIRE library=cpp semantic=sem-acquire traits=semaphore
__pthread_sem_post TD_SEMAPHORE_RELEASE library=cpp semantic=sem-release traits=semaphore
