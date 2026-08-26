idf_component_register(
    SRCS "src/ui_core.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES heap log
    REQUIRED_IDF_TARGETS esp32p4 esp32s3
)

set(POCKETJS_RUST_FROM_SOURCE OFF CACHE BOOL
    "Build the PocketJS native archive with the user-provided Rust toolchain")
set(POCKETJS_PREBUILT_ARCHIVE "" CACHE FILEPATH
    "Override the target-specific PocketJS native archive (development and CI)")

if(IDF_TARGET STREQUAL "esp32p4")
    set(pocketjs_rust_target "riscv32imafc-unknown-none-elf")
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(pocketjs_rust_target "xtensa-esp32s3-none-elf")
else()
    message(FATAL_ERROR "pocketjs_ui_core supports esp32p4 and esp32s3")
endif()

if(POCKETJS_PREBUILT_ARCHIVE)
    get_filename_component(prebuilt "${POCKETJS_PREBUILT_ARCHIVE}" ABSOLUTE)
else()
    set(prebuilt "${CMAKE_CURRENT_LIST_DIR}/lib/${IDF_TARGET}/libpocketjs_idf_native.a")
endif()
if(POCKETJS_RUST_FROM_SOURCE)
    find_program(POCKETJS_CARGO cargo)
    if(NOT POCKETJS_CARGO)
        message(FATAL_ERROR
            "POCKETJS_RUST_FROM_SOURCE requires cargo and target ${pocketjs_rust_target}; PocketJS never installs Rust")
    endif()
    get_filename_component(pocketjs_cargo_dir "${POCKETJS_CARGO}" DIRECTORY)
    find_program(POCKETJS_RUSTC NAMES rustc
        HINTS "${pocketjs_cargo_dir}" NO_DEFAULT_PATH)
    if(NOT POCKETJS_RUSTC)
        message(FATAL_ERROR
            "POCKETJS_RUST_FROM_SOURCE requires rustc next to ${POCKETJS_CARGO}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/vendor/native/Cargo.toml")
        set(native_root "${CMAKE_CURRENT_LIST_DIR}/vendor/native")
        set(core_source_root "${CMAKE_CURRENT_LIST_DIR}/vendor/core")
        set(renderer_source_root "${CMAKE_CURRENT_LIST_DIR}/vendor/esp32p4-ppa")
    else()
        set(native_root "${CMAKE_CURRENT_LIST_DIR}/../../native")
        set(core_source_root "${CMAKE_CURRENT_LIST_DIR}/../../../../engine/core")
        set(renderer_source_root "${CMAKE_CURRENT_LIST_DIR}/../../../../engine/backends/esp32p4-ppa")
    endif()
    set(cargo_target "${CMAKE_CURRENT_BINARY_DIR}/cargo")
    set(cargo_archive
        "${cargo_target}/${pocketjs_rust_target}/release/libpocketjs_idf_native.a")
    set(native_archive
        "${cargo_target}/prepared/${pocketjs_rust_target}/libpocketjs_idf_native.a")
    set(prepare_archive "${CMAKE_CURRENT_LIST_DIR}/prepare_archive.cmake")
    file(GLOB_RECURSE native_sources CONFIGURE_DEPENDS
        "${native_root}/src/*.rs"
        "${native_root}/Cargo.toml"
        "${native_root}/Cargo.lock"
        "${core_source_root}/src/*.rs"
        "${core_source_root}/Cargo.toml"
        "${renderer_source_root}/src/*.rs"
        "${renderer_source_root}/Cargo.toml")
    set(pocketjs_cargo_unstable)
    if(IDF_TARGET STREQUAL "esp32s3")
        list(APPEND pocketjs_cargo_unstable "-Zbuild-std=core,alloc")
    endif()
    add_custom_command(
        OUTPUT "${native_archive}"
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARGO_TARGET_DIR=${cargo_target}" "RUSTC=${POCKETJS_RUSTC}"
            "${POCKETJS_CARGO}" build ${pocketjs_cargo_unstable}
            --release --locked --no-default-features
            --target "${pocketjs_rust_target}" --manifest-path "${native_root}/Cargo.toml"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${cargo_target}/prepared/${pocketjs_rust_target}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${cargo_archive}" "${native_archive}"
        COMMAND "${CMAKE_COMMAND}"
            "-DPOCKETJS_ARCHIVE=${native_archive}"
            "-DPOCKETJS_ARCHIVER=${CMAKE_AR}"
            "-DPOCKETJS_TARGET=${IDF_TARGET}"
            -P "${prepare_archive}"
        DEPENDS ${native_sources} "${prepare_archive}"
        VERBATIM)
    add_custom_target(pocketjs_native_build DEPENDS "${native_archive}")
else()
    if(NOT EXISTS "${prebuilt}")
        message(FATAL_ERROR
            "PocketJS prebuilt archive is missing for ${IDF_TARGET}; install a Registry release or set POCKETJS_RUST_FROM_SOURCE=ON")
    endif()
    set(native_archive "${prebuilt}")
endif()

add_library(pocketjs_native STATIC IMPORTED GLOBAL)
set_target_properties(pocketjs_native PROPERTIES IMPORTED_LOCATION "${native_archive}")
if(POCKETJS_RUST_FROM_SOURCE)
    add_dependencies(pocketjs_native pocketjs_native_build)
endif()
target_link_libraries(${COMPONENT_LIB} PUBLIC pocketjs_native)
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=pocketjs_idf_rust_alloc"
    "-Wl,--undefined=pocketjs_idf_rust_dealloc"
    "-Wl,--undefined=pocketjs_idf_rust_panic")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
