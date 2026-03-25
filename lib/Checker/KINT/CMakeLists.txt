add_library(CanaryKint STATIC
    MKintPass.cpp
    MKintPass_bugreport.cpp
    RangeAnalysis.cpp
    KINTTaintAnalysis.cpp
    BugDetection.cpp
    SmtMemory.cpp
    Utils.cpp
    Options.cpp
    Log.cpp
)