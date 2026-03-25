/**
 * @file SMTSampler.h
 * @brief Main header for SMT sampling functionality
 *
 * This file provides the common includes and forward declarations for the SMT
 * sampling module.  The module implements various techniques for sampling
 * satisfying assignments (models) from SMT formulas, including:
 *
 * 1. QuickSampler: A mutation-based approach for generating diverse models.
 * 2. RegionSampler (PolySampler): A geometry-based approach for sampling from
 *    convex polytopes defined by linear constraints.
 * 3. IntervalSampler: An interval-based sampling strategy.
 *
 * These samplers are used for test case generation, solution space exploration,
 * and analyzing formula sensitivity.
 *
 * NOTE (L8 fix): "using namespace std" and "using namespace z3" have been
 * removed from this header.  They were polluting the namespace of every
 * translation unit that included this file.  Each .cpp file that needs these
 * namespaces should declare them locally.
 */

#pragma once

#include "Solvers/SMT/LIBSMT/Z3Plus.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <z3++.h>

// Fix L8: do NOT place "using namespace std" or "using namespace z3" here.
// Each .cpp implementation file declares them locally as needed.

// Forward declarations for sampler implementations
class quick_sampler;
struct interval_sampler;
struct region_sampler;
