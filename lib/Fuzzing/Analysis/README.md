

from https://github.com/vusec/libaflgo

It re-implements three directed fuzzing policies in a modular fashion: AFLGo, Hawkeye, and DAFL. 

- CCS 17: Directed Greybox Fuzzing
- CCS 18: Hawkeye: Towards a desired directed grey-box fuzzer
- USENIX Security 23: DAFL: Directed Grey-box Fuzzing guided by Data Dependency


The analyses:
 
- BasicBlockDistance.hpp                      <-     AFLGo basic block distance analysis
- DAFL.hpp                                    <-     DAFL data-flow distance
- ExtendedCallGraph.hpp                        <-     enhance CFG with PTA
- FunctionDistance.hpp                        <-     Hawkeye function distance analysis
- TargetDetection.hpp                         <-     supporting target instrumentation