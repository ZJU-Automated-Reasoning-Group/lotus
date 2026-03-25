/**
 * @file PDGQuery.h
 * @brief Umbrella include for the split PDG query interface.
 *
 * This header is intentionally thin. It preserves the original single-include
 * experience for clients while delegating the actual declarations to the
 * focused headers introduced by the query-layer split:
 * - PDGQueryCore.h: shared vocabulary and core query services
 * - SummaryQuery.h: function summary extraction
 * - ImpactQuery.h: change/impact ranking over PDG reachability
 * - ResourceFlowQuery.h: built-in resource acquire/release tracking
 */

#pragma once

#include "IR/PDG/Analysis/ImpactQuery.h"
#include "IR/PDG/Analysis/PDGQueryCore.h"
#include "IR/PDG/Analysis/ResourceFlowQuery.h"
#include "IR/PDG/Analysis/SummaryQuery.h"
