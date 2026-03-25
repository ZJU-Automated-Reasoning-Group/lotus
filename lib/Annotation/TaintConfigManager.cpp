/*
 * Taint Configuration Manager Implementation
 *
 * The singleton instance is now a function-local static inside
 * TaintConfigManager::getInstance() (defined in the header), which is
 * guaranteed to be initialised exactly once in a thread-safe manner by the
 * C++11 standard.  No out-of-line static member definition is required.
 */

#include "Annotation/Taint/TaintConfigManager.h"
