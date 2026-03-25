#ifndef UTILS_TIMER_H
#define UTILS_TIMER_H

#include <functional>

#include <time.h>

class Timer {
private:
  double Duration;
  size_t Steps, StepsCounter;
  double SuspendTime;
  bool Suspended;
  time_t StartTime;
  time_t SuspendStartTime;

  std::function<void()> TaskAfterTimeout;

public:
  /// The constructor should set the duration \p D of the timer
  /// and the task \p T that should take after timeout happens.
  /// The last parameter \S is an optimization which controls
  /// the frequency you check timeout. For example, if \p S is
  /// set to 5, you will really check timeout once you call
  /// Timer::check() five times.
  template <typename Task>
  Timer(double D, Task T, unsigned S = 0)
      : Duration(D), Steps(S), StepsCounter(S), SuspendTime(0.0),
        Suspended(false) {
    time(&StartTime);
    time(&SuspendStartTime);
    TaskAfterTimeout = [T]() { T(); };
  }

  // Suspend/resume timer, the time consumption between suspend() and resume()
  // shall be ignored
  void suspend();
  void resume();

  // return true if time out exists
  bool isTimeOut();

  // Check timeout and perform the corresponding actions
  void check();
};

#endif /* UTILS_TIMER_H */