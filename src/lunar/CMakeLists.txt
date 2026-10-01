add_executable(lunar
    src/main.cpp
    src/cli_text.cpp
)

target_link_libraries(lunar
    PRIVATE
        okrapmlib
)

target_compile_features(lunar PRIVATE cxx_std_17)

install(TARGETS lunar DESTINATION bin)
