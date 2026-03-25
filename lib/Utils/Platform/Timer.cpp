#include "Utils/Platform/Timer.h"

// Suspends the timer, recording the suspend time.
void Timer::suspend() {
  if (!Suspended) {
    time(&SuspendStartTime);
    Suspended = true;
  }
}

// Resumes the timer, adding suspend time to total.
void Timer::resume() {
  if (Suspended) {
    time_t CurrTime;
    time(&CurrTime);
    double TimeElapsed = difftime(CurrTime, SuspendStartTime);
    SuspendTime += TimeElapsed;
    Suspended = false;
  }
}

// Returns true if the timer has exceeded its duration or step limit.
bool Timer::isTimeOut() {
  if (StepsCounter == 0) {
    StepsCounter = Steps;

    double TimeElapsed;

    if (Suspended) {
      TimeElapsed = difftime(SuspendStartTime, StartTime);
    } else {
      time_t CurrTime;
      time(&CurrTime);
      TimeElapsed = difftime(CurrTime, StartTime);
    }

    if (TimeElapsed > (Duration + SuspendTime)) {
      return true;
    }
  } else {
    StepsCounter--;
  }

  return false;
}

// Checks for timeout and executes callback if needed.
void Timer::check() {
  if (isTimeOut()) {
    TaskAfterTimeout();
  }
}