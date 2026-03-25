#pragma once
#include "Checker/FiTx/Frontend/StateTransition.h"

#include <string>
#include <vector>

const std::vector<std::string> alloc_funcs = {
    "malloc",        "calloc",           "kzalloc",
    "kmalloc",       "zalloc",           "vmalloc",
    "kcalloc",       "vzalloc",          "kzalloc_node",
    "kmalloc_array", "kmem_cache_alloc", "kmem_cache_alloc_node",
    /* "memdup",        "kmemdup",          "kstrdup" */
};

const std::vector<std::string> free_funcs = {
    "free",   "kfree",           "kzfree",         "vfree",
    "kvfree", "kmem_cache_free", "kfree_sensitive"};

const std::vector<fitx::FunctionArgTransitionRule::FunctionArg>
    store_related = {
        fitx::FunctionArgTransitionRule::FunctionArg(
            "__drm_atomic_helper_crtc_reset", 1, true),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "__drm_atomic_helper_connector_reset", 1, true),
        fitx::FunctionArgTransitionRule::FunctionArg("list_add_tail", 0,
                                                          true),
        fitx::FunctionArgTransitionRule::FunctionArg("list_add", 0, true),
};
