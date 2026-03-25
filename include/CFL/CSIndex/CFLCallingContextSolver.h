#pragma once

#include <cassert>
#include <cstddef>
// #include <unordered_map>
// #include <unordered_set>
#include <vector>

/// A parenthesis is represented as a non-zero integer. A left-parenthesis
/// is a positive integer and a right-parenthesis is a negative integer.
///
/// When we visit a call/ret edge during a depth-first search, we can use
/// the method "bool add(int)" to add the parenthsis labeled on the edge.
/// If the method returns false, it means it is not context sensitive to
/// continue the search.
///
/// As the above push/pop utilities, the push() method is to record the
/// backtrace point and the pop() method is to jump back the backtrace point.
class CFLCallingContextSolver {
protected:
  typedef struct Item {
    int PrevIndex;
    int NextIndex;
    int SelfIndex;
    int Label;

    Item(int p, int n, int s, int l)
        : PrevIndex(p), NextIndex(n), SelfIndex(s), Label(l) {}

    ~Item() {}
  } Item;

  std::vector<size_t> SizeStack;
  std::vector<Item> LabelVector;
  size_t CallingDepth;

public:
  CFLCallingContextSolver() {
    // push a base
    LabelVector.emplace_back(-1, 1, 0, 0);
    LabelVector.emplace_back(0, 2, 1, 0);
    CallingDepth = 0;
  }

  virtual ~CFLCallingContextSolver() {}

  virtual bool add(int N) {
    assert(N != 0 && "Zero is not regarded as a label!");
    if (N > 0) {
      LabelVector.back().Label = N;

      int CurSize = LabelVector.size();
      LabelVector.emplace_back(CurSize - 1, CurSize + 1, CurSize, 0);

      CallingDepth++;
    } else {
      // N < 0
      if (LabelVector[LabelVector.back().PrevIndex].Label <= 0) {
        // no need to match
        LabelVector.back().Label = N;

        int CurSize = LabelVector.size();
        LabelVector.emplace_back(CurSize - 1, CurSize + 1, CurSize, 0);

        CallingDepth++;
      } else {
        // need to match
        Item &Label2MatchItem = LabelVector[LabelVector.back().PrevIndex];
        if (Label2MatchItem.Label + N == 0) {
          // can match
          LabelVector.back().Label = N;

          int CurSize = LabelVector.size();
          LabelVector.emplace_back(CurSize - 1, CurSize + 1, CurSize, 0);

          LabelVector.back().PrevIndex =
              LabelVector[Label2MatchItem.PrevIndex].SelfIndex;
          LabelVector[Label2MatchItem.PrevIndex].NextIndex = CurSize;

          CallingDepth--;
        } else {
          // cannot match
          return false;
        }
      }
    }
    return true;
  }

  virtual void push() { SizeStack.push_back(LabelVector.size()); }

  virtual void pop() {
    assert(!SizeStack.empty() &&
           "The push and pop operations are mis-matched!");
    size_t TargetSz = SizeStack.back();
    SizeStack.pop_back();

    if (TargetSz == LabelVector.size())
      return;

    assert(TargetSz < LabelVector.size());
    while (TargetSz < LabelVector.size()) {
      auto PopedItem = LabelVector.back();
      LabelVector.pop_back();

      // reset the top
      auto &CurrTopItem = LabelVector.back();
      CurrTopItem.Label = 0;

      if ((size_t)LabelVector[PopedItem.PrevIndex].SelfIndex ==
          LabelVector.size() - 1) {
        CallingDepth--;
      } else {
        CallingDepth++;
        LabelVector[PopedItem.PrevIndex].NextIndex =
            LabelVector[CurrTopItem.PrevIndex].SelfIndex;
      }
    }
  }

  virtual void reset() { // clear
    SizeStack.clear();
    LabelVector.clear();

    LabelVector.emplace_back(-1, 1, 0, 0);
    LabelVector.emplace_back(0, 2, 1, 0);
    CallingDepth = 0;
  }

  bool empty() const {
    return LabelVector.size() == 2 && LabelVector[1].Label == 0;
  }

  size_t callingDepth() const { return CallingDepth; }
};
