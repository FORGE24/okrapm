add_library(opsislib
	src/engine.cpp
)

target_include_directories(opsislib
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opsislib PUBLIC cxx_std_17)

add_executable(opsis
	src/main.cpp
)

target_link_libraries(opsis
	PRIVATE
		opsislib
)

install(TARGETS opsis DESTINATION bin)
