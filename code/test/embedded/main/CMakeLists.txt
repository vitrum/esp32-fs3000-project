idf_component_register(
    SRCS "test_runtime.cpp" "../../../src/fit_activity.cpp"
    INCLUDE_DIRS "../../../src"
    REQUIRES unity freertos
)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)