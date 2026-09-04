add_executable(csr csr.cpp)
target_link_libraries(csr PRIVATE
    CanaryCSIndexFLARE
)
