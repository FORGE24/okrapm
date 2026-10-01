add_library(okrapmlib
    src/version.cpp
    src/object.cpp
    src/object_ref.cpp
    src/operation.cpp
    src/transaction.cpp
    src/repository.cpp
    src/resolver.cpp
    src/system_store.cpp
    src/snapshot.cpp
    src/extension_api.cpp
    src/artifact_engine.cpp
    src/network_downloader.cpp
    src/pipeline_engine.cpp
    src/lunar_core.cpp
)

target_include_directories(okrapmlib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(okrapmlib
    PUBLIC
        ${CMAKE_DL_LIBS}
        opsislib
)

target_compile_features(okrapmlib PUBLIC cxx_std_17)
