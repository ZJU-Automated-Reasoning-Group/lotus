# SymAbsAI analysis tool

add_executable(symabs_ai spranalyze.cpp)

llvm_map_components_to_libnames(symabs_ai_tool_llvm_libs
    core irreader bitreader)

target_link_libraries(symabs_ai PRIVATE
    SymAbsAIAnalysis
    ${symabs_ai_tool_llvm_libs}
    ${Z3_LIBRARIES}
)

# Ensure all registration objects from the static lib are linked (avoid dead stripping)
if(APPLE)
  # On macOS, force loading of all object files from static archives
  target_link_options(symabs_ai PRIVATE -Wl,-all_load)
endif()

install(TARGETS symabs_ai DESTINATION bin)
