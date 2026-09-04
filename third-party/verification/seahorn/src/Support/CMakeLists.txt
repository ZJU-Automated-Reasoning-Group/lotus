add_library (SeaSupport STATIC
  SortTopo.cc
  Stats.cc
  Profiler.cc
  CFGPrinter.cc
  SeaLog.cc
  SeaDebug.cc
  ${SEAHORN_GIT_SHA1_SOURCE}
  BoostException.cc
  )
