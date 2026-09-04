/**
 * @file seahorn.h
 * @brief Seahorn verifier functions and intrinsics
 *
 * This header provides Seahorn verifier functions for symbolic execution
 * and bounded model checking, including assertion checking, memory tracking,
 * and shadow memory management.
 *
 * @author Lotus Analysis Framework
 */

#ifndef _SEAHORN__H_
#define _SEAHORN__H_

#include <seahorn/ownsem.h>
#include <stdbool.h>
#include <stdint.h>

#define SEA_NONDET_FN_ATTR __declspec(noalias)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Marks an error location for the verifier
///
/// Catastrophic failure that matters.
extern void __VERIFIER_error(void);

/// @brief A condition to be assumed to be true by the verifier
/// @param condition The condition to assume
extern void __VERIFIER_assume(int);
extern void __SEA_assume(bool);

/// @brief Assert a condition
/// @param cond The condition to assert
extern void __VERIFIER_assert(bool);
extern void __VERIFIER_assert_not(bool);
extern void __VERIFIER_assert_if(bool, bool);

/// @brief Check if memory at ptr + offset is dereferenceable
/// @param ptr The pointer to check
/// @param offset The offset in bytes from ptr
/// @return TRUE if offset number of bytes of ptr are allocated
///
/// Requires support from the memory manager. Might be interpreted to always
/// return TRUE or FALSE if the memory manager does not support it.
#define sea_is_deref sea_is_dereferenceable
extern bool sea_is_dereferenceable(const void *ptr, intptr_t offset);

/// @brief Conditional assertion
/// @param cond1 Condition to check
/// @param cond2 Condition to assert if cond1 is true
extern void sea_assert_if(bool, bool);

/// @brief Check if memory has been modified
/// @param arg The memory pointer to check
/// @return true if memory pointed to by arg has been modified from:
///         1. allocation OR
///         2. reset_modified OR
///         3. sea_tracking_on
///         whichever is the latest event.
extern bool sea_is_modified(char *);

/// @brief Enable memory tracking for subsequent program until exit or
/// sea_tracking_off
extern void sea_tracking_on(void);

/// @brief Disable memory tracking for subsequent program until exit or
/// sea_tracking_on
extern void sea_tracking_off(void);

/// @brief Reset modified metadata for memory pointed to by arg
/// @param arg The memory pointer to reset
extern void sea_reset_modified(char *);

/// @name Shadow memory operations
/// @brief Set and get values from shadow memory slots
/// @{
/// @brief Set a shadow memory slot S at addr A with value V.
/// @param S Shadow memory slot (0 is main memory and should not be used)
/// @param A The address
/// @param V The value to set
extern void sea_set_shadowmem(char, char *, size_t);

/// @brief Get a value from shadow memory slot S at address A.
/// @param S Shadow memory slot (0 is main memory and should not be used)
/// @param A The address
/// @return The value stored in the shadow memory slot
extern size_t sea_get_shadowmem(char, char *);
/// @}

#define TRACK_READ_MEM 0
#define TRACK_WRITE_MEM 1
#define TRACK_ALLOC_MEM 2
#define TRACK_CUSTOM0_MEM 3
#define TRACK_CUSTOM1_MEM 4

#ifdef __cplusplus
}
#endif

/* Convenience macros */
#define assume __SEA_assume

#ifdef VACCHECK
/* See https://github.com/seahorn/seahorn/projects/5 for details */
#define sassert(X)                                                             \
  (void)((__VERIFIER_assert(X), (X)) || (__VERIFIER_error(), 0))
#elif defined(SEA_SYNTH)
/* See test/synth/ for use cases */
#define PARTIAL_FN                                                             \
  __attribute__((annotate("partial"))) __attribute__((noinline))
#define sassert(X)                                                             \
  (void)((__VERIFIER_assert(X), (X)) || (__VERIFIER_error(), 0))
#else
/* Default semantics of sassert */
#define sassert(X) (void)((X) || (__VERIFIER_error(), 0))
#endif

#endif
