#include "Utils/LLVM/RecursiveTimer.h"

// Current depth of nested timers for indentation.
static unsigned DepthOfTimeRecorder = 0;
bool RecursiveTimer::Enabled = true;

// Returns a string of N indentation spaces.
static inline std::string Tab(unsigned N) {
  std::string Ret;
  while (N-- > 0)
    Ret.append("    ");
  return Ret;
}

// Constructs a timer with a C-string prefix.
RecursiveTimer::RecursiveTimer(const char *Prefix)
    : Begin(std::chrono::steady_clock::now()), Prefix(Prefix) {
  if (!Enabled)
    return;
  Active = true;
  outs() << Tab(DepthOfTimeRecorder++) << Prefix << "...\n";
}

// Constructs a timer with a string prefix.
RecursiveTimer::RecursiveTimer(const std::string &Prefix)
    : Begin(std::chrono::steady_clock::now()), Prefix(Prefix) {
  if (!Enabled)
    return;
  Active = true;
  outs() << Tab(DepthOfTimeRecorder++) << Prefix << "...\n";
}

// Destructor prints the elapsed time.
RecursiveTimer::~RecursiveTimer() {
  if (!Active)
    return;
  std::chrono::steady_clock::time_point End = std::chrono::steady_clock::now();
  auto Milli =
      std::chrono::duration_cast<std::chrono::milliseconds>(End - Begin)
          .count();
  auto Time = Milli > 1000 ? Milli / 1000 : Milli;
  const auto *Unit = Milli > 1000 ? "s" : "ms";
  outs() << Tab(--DepthOfTimeRecorder) << Prefix << " takes " << Time << Unit
         << "!\n";
}

// LLVM pass ID for RecursiveTimerPass.
char RecursiveTimerPass::ID = 0;
