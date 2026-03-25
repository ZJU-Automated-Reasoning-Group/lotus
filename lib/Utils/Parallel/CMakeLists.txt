add_library(CanaryParallel STATIC
    ThreadPool.cpp
    Scheduler/Task.cpp
    Scheduler/PipelineScheduler.cpp
    Scheduler/ParallelSchedulerPass.cpp
)

target_link_libraries(CanaryParallel PUBLIC LLVMAnalysis CanarySupport)
