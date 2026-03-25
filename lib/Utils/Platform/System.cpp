#include "Utils/Platform/System.h"

#include <chrono>
#include <thread>
// #include "Utils/General/Options.h"

#ifdef OS_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <ctime>
#include <iostream>
#include <random>

#ifdef __linux__

#include <stdio.h>
#include <string.h>
bool isTraced() {
  char buf1[512];
  FILE *fin;
  fin = fopen("/proc/self/status", "r");

  if (!fin) {
    return false;
  }

  bool ret = false;
  int tpid;
  const char *needle = "TracerPid:";
  size_t nl = strlen(needle);
  while (fgets(buf1, 512, fin)) {
    if (!strncmp(buf1, needle, nl)) {
      sscanf(buf1, "TracerPid: %d", &tpid);
      if (tpid != 0) {
        ret = true;
        break;
      }
    }
  }
  fclose(fin);
  return ret;
}

#elif _WIN32

#include <windows.h>
bool isTraced() {
  if (IsDebuggerPresent()) {
    return true;
  }
  return false;
}

#else
bool isTraced() { return false; }

#endif

/**
 * @brief Finds out if the runtime architecture is little endian.
 */
bool isLittleEndian() {
  // We use static variables to compute the endianess only once.
  static const short endian_test_pattern = 0x00ff;
  static const bool little_endian =
      *(reinterpret_cast<const char *>(&endian_test_pattern)) == '\xff';
  return little_endian;
}

/**
 * @brief Finds out if the runtime system supports <tt>long double</tt> (at
 * least 10 bytes long).
 */
bool systemHasLongDouble() { return sizeof(long double) >= 10; }

// void getcwdPP(char * buf, int size) {
//
// #ifdef __APPLE__
//     unsigned s = size;
//     _NSGetExecutablePath(buf, &s);
// #else
//     int s = readlink("/proc/self/exe", buf, size);
// #endif
// }

int random_int(int limit) {

  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_int_distribution<int> dist(0, limit);

  return dist(mt);
}

double random_real(double limit) {

  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_real_distribution<double> dist(0, limit);

  return dist(mt);
}

/**
 * @brief Returns how much time has elapsed since the program was started
 *        (in seconds).
 */
double getElapsedTime() {

// Disable the -Wold-style-cast warning on Windows because CLOCKS_PER_SEC
// on Windows contains a C cast. This warning is only for GCC (MSVC does
// not support it).
#if defined(OS_WINDOWS) && defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

  /// STATIC
  static auto start = std::chrono::steady_clock::now();
  //    auto start = std::chrono::high_resolution_clock::now();

  auto now = std::chrono::steady_clock::now();
  //    auto now = std::chrono::high_resolution_clock::now();
  double consume =
      (double)std::chrono::duration_cast<std::chrono::microseconds>(now - start)
          .count() /
      CLOCKS_PER_SEC;
  return consume;
  // return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

/**
 * @brief Returns max number of thread to use.
 */
unsigned getMaxNumOfThread() {

  //    return 1;
  unsigned int n_threads = std::thread::hardware_concurrency();
  unsigned int n_cores = 4;
  if (n_cores > 0 && n_cores <= n_threads)
    return n_cores;

  return n_threads >= 2 ? n_threads - 1 : n_threads;
}

double profileGetTime() {
#ifdef PROFILE
  return getElapsedTime();
#else
  return 0;
#endif
}
