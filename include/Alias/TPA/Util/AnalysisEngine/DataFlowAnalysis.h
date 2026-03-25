#pragma once

namespace util {

template <typename GlobalState, typename Memo, typename TransferFunction,
          typename Propagator>
class DataFlowAnalysis {
private:
  GlobalState &globalState;
  Memo &memo;

public:
  DataFlowAnalysis(GlobalState &g, Memo &m) : globalState(g), memo(m) {}

  DataFlowAnalysis(const DataFlowAnalysis &) = delete;
  DataFlowAnalysis(DataFlowAnalysis &&) noexcept = default;
  DataFlowAnalysis &operator=(const DataFlowAnalysis &) = delete;
  DataFlowAnalysis &operator=(DataFlowAnalysis &&) = delete;

  template <typename WorkList> void runOnWorkList(WorkList workList) {
    while (!workList.empty()) {
      auto item = workList.dequeue();
      auto localState = memo.lookup(item);
      auto evalResult = TransferFunction(globalState, localState).eval(item);
      Propagator(memo, workList).propagate(evalResult);
    }
  }

  template <typename Initializer, typename InitialState>
  void runOnInitialState(InitialState &&initState) {
    auto workList = Initializer(globalState, memo)
                        .runOnInitState(std::forward<InitialState>(initState));
    runOnWorkList(std::move(workList));
  }
};

} // namespace util
