cmake_minimum_required(VERSION 3.16)
project(okrapm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(opsis)
add_subdirectory(lib/okrapmlib)
add_subdirectory(src/lunar)

enable_testing()
enable_language(C)

add_library(oaabi_sample MODULE tests/plugins/oaabi_sample.c)
target_include_directories(oaabi_sample PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(oaabi_sample PROPERTIES PREFIX "" OUTPUT_NAME oaabi_sample)

add_library(oaabi_bad MODULE tests/plugins/oaabi_bad.c)
target_include_directories(oaabi_bad PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(oaabi_bad PROPERTIES PREFIX "" OUTPUT_NAME oaabi_bad)

add_library(legacy_plugin MODULE tests/plugins/legacy_plugin.cpp)
set_target_properties(legacy_plugin PROPERTIES PREFIX "" OUTPUT_NAME legacy_plugin)

add_executable(lunar_test tests/test_all.cpp)
target_link_libraries(lunar_test PRIVATE okrapmlib)
target_compile_definitions(lunar_test PRIVATE
	"LEGACY_PLUGIN_PATH=\"$<TARGET_FILE:legacy_plugin>\""
	"OAABI_PLUGIN_PATH=\"$<TARGET_FILE:oaabi_sample>\""
	"OAABI_BAD_PLUGIN_PATH=\"$<TARGET_FILE:oaabi_bad>\""
)
add_dependencies(lunar_test legacy_plugin oaabi_sample oaabi_bad)
add_test(NAME LunarTest COMMAND lunar_test)
add_test(
    NAME HostRepoLoop
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/host-repo-loop.sh $<TARGET_FILE:lunar>
)
add_executable(opsis_test tests/opsis_test.cpp)
target_link_libraries(opsis_test PRIVATE opsislib okrapmlib)
add_test(NAME OpsisTest COMMAND opsis_test)

include(${CMAKE_SOURCE_DIR}/cmake/OpsisPackage.cmake)
opsis_direct_package(
	NAME okrapm
	NAMESPACE Okra
	VERSION ${PROJECT_VERSION}
	DESCRIPTION "Okra package manager"
	SCRIPT ${CMAKE_SOURCE_DIR}/package/okrapm.opsis
	OUTPUT ${CMAKE_BINARY_DIR}/Okra.okrapm.oaa
	TARGETS lunar opsis
)
