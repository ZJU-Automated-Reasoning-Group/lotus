/* Author: Anshunkang Zhou <azhouad@cse.ust.hk>
 * File Description:
 * Creation Date: July 06, 2019
 * Modification History:
 */

#ifndef PLANKTON_DASM_SYSTEM_H
#define PLANKTON_DASM_SYSTEM_H

#include <cstring>

// Obtain the used operating system. Currently, we only distinguish between
// Windows, macOS, and Linux.
#if defined(__WIN) || defined(_WIN32) || defined(__WIN32__) ||                 \
    defined(__CYGWIN__)
#define OS_WINDOWS
#else
#include <sys/param.h>
#if defined(__APPLE__)
#define OS_MACOS
#elif defined(BSD)
#define OS_BSD
#else
#define OS_LINUX
#endif
#endif

// It is also useful to know whether the operating system is POSIX compliant.
#if defined(OS_MACOS) || defined(OS_LINUX) || defined(OS_BSD)
#define OS_POSIX
#endif

/// infinite large number
#define INF 999999999999

template <typename T> void releaseContainer(T &c) {
  c.clear();
  T().swap(c);
}

bool isTraced();
bool isLittleEndian();
bool systemHasLongDouble();
void getcwdPP(char *buf, int size);
int random_int(int limit);
double random_real(double limit);
double getElapsedTime();
unsigned getMaxNumOfThread();
double profileGetTime();

#endif // PLANKTON_DASM_SYSTEM_H