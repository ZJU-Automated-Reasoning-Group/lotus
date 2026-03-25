# Concurrency checker library
add_library(ConcurrencyChecker STATIC
    ConcurrencyChecker.cpp
    ConcurrencyAnalysisDumper.cpp
    DataRaceChecker.cpp
    DeadlockChecker.cpp
    AtomicityChecker.cpp
    ConditionVariableChecker.cpp
    LockMismatchChecker.cpp
    OpenMPChecker.cpp
    MPIChecker.cpp
)

target_link_libraries(ConcurrencyChecker
    PUBLIC
        Concurrency  # MHP and LockSet analysis
)
