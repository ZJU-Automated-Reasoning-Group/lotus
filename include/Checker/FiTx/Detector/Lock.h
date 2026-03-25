#pragma once
#include "Checker/FiTx/Frontend/StateTransition.h"

#include <string>
#include <vector>

const std::vector<fitx::FunctionArgTransitionRule::FunctionArg>
    lock_funcs = {
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock"),
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock_irq"),
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock_irqsave"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_lock"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_lock_nested"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_interruptible"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_interruptible_nested"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_killable"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "refcount_dec_and_mutex_lock", 1),
        fitx::FunctionArgTransitionRule::FunctionArg("atomic_dec_and_lock",
                                                          1),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "_atomic_dec_and_lock", 1),
};

const std::vector<fitx::FunctionArgTransitionRule::FunctionArg>
    try_lock_funcs = {
        fitx::FunctionArgTransitionRule::FunctionArg("spin_trylock"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_trylock"),
};

const std::vector<fitx::FunctionArgTransitionRule::FunctionArg>
    lock_funcs_w_try = {
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock"),
        fitx::FunctionArgTransitionRule::FunctionArg("spin_trylock"),
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock_irq"),
        fitx::FunctionArgTransitionRule::FunctionArg("spin_lock_irqsave"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_lock"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_lock_nested"),
        fitx::FunctionArgTransitionRule::FunctionArg("mutex_trylock"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_interruptible"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_interruptible_nested"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "mutex_lock_killable"),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "refcount_dec_and_mutex_lock", 1),
        fitx::FunctionArgTransitionRule::FunctionArg("atomic_dec_and_lock",
                                                          1),
        fitx::FunctionArgTransitionRule::FunctionArg(
            "_atomic_dec_and_lock", 1),
};

const std::vector<std::string> unlock_funcs = {
    "spin_unlock", "spin_unlock_irq", "spin_unlock_irqsave", "mutex_unlock"};
