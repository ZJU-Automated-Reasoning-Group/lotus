/*
 *  Canary features a fast unification-based alias analysis for C programs
 *  Copyright (C) 2021 Qingkai Shi <qingkaishi@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as published
 *  by the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef SUPPORT_RECURSIVETIMER_H
#define SUPPORT_RECURSIVETIMER_H

#include <chrono>
#include <string>
#include <utility>

#include <llvm/Pass.h>
#include <llvm/Support/raw_os_ostream.h>

using namespace llvm;

class RecursiveTimer {
private:
  std::chrono::steady_clock::time_point Begin;
  std::string Prefix;
  bool Active = false;
  static bool Enabled;

public:
  /// the prefix should be in a style of "Doing sth" or "Sth"
  /// @{
  explicit RecursiveTimer(const char *Prefix);

  explicit RecursiveTimer(const std::string &Prefix);
  /// @}

  static void setEnabled(bool Enable) { Enabled = Enable; }
  static bool isEnabled() { return Enabled; }

  /// end of the recorder
  ~RecursiveTimer();
};

class RecursiveTimerPass : public ModulePass {
private:
  RecursiveTimer **TimerPtr;
  std::string Message;
  bool Internal;

public:
  static char ID;

  explicit RecursiveTimerPass(const char *Prefix)
      : ModulePass(ID), TimerPtr(new RecursiveTimer *), Message(Prefix),
        Internal(false) {
    *TimerPtr = nullptr;
  }

  explicit RecursiveTimerPass(std::string Prefix)
      : ModulePass(ID), TimerPtr(new RecursiveTimer *),
        Message(std::move(Prefix)), Internal(false) {
    *TimerPtr = nullptr;
  }

  ~RecursiveTimerPass() override {
    if (!Internal)
      delete TimerPtr;
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  bool runOnModule(Module &) override {
    if (!*TimerPtr) {
      *TimerPtr = new RecursiveTimer(Message);
    } else {
      delete *TimerPtr;
      *TimerPtr = nullptr;
    }
    return false;
  }

private:
  explicit RecursiveTimerPass(RecursiveTimer **Timer)
      : ModulePass(ID), TimerPtr(Timer), Internal(true) {}

public:
  Pass *start() { return this; }

  Pass *done() { return new RecursiveTimerPass(TimerPtr); }
};

#endif // SUPPORT_RECURSIVETIMER_H
