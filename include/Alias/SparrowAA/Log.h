/**
 * @file Log.h
 * @brief Logging macros for SparrowAA alias analysis
 *
 * This file provides a set of logging macros for the SparrowAA alias analysis
 * system, built on top of spdlog. It offers multiple log levels including
 * trace, debug, info, warning, and error for different verbosity levels.
 *
 * @ingroup SparrowAA
 */

#ifndef ANDERSEN_LOGGING_H
#define ANDERSEN_LOGGING_H

#include <spdlog/spdlog.h>

#define LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

#endif
