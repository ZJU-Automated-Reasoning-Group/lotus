# spdlog (header-only). Consumers use #include <spdlog/...> with third-party on include path.
add_library(spdlog INTERFACE)
target_include_directories(spdlog INTERFACE
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
	$<INSTALL_INTERFACE:include>
)
add_library(spdlog::spdlog ALIAS spdlog)
