include_guard(GLOBAL)

set(POCKETJS_PACKAGE_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(POCKETJS_PACKAGE_EMBED_TOOL
    "${POCKETJS_PACKAGE_COMPONENT_DIR}/tools/embed_package.py")

function(_pocketjs_package_python out)
    if(DEFINED PYTHON AND EXISTS "${PYTHON}")
        set(${out} "${PYTHON}" PARENT_SCOPE)
        return()
    endif()
    find_program(pocketjs_python NAMES python3 python REQUIRED)
    set(${out} "${pocketjs_python}" PARENT_SCOPE)
endfunction()

function(_pocketjs_package_attach target name package profile)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "PocketJS package target '${target}' does not exist")
    endif()
    if(NOT "${name}" MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "PocketJS package NAME must match [a-z][a-z0-9_]*")
    endif()
    _pocketjs_package_python(python)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/pocketjs/${name}")
    set(stem "pocketjs_package_${name}")
    add_custom_command(
        OUTPUT "${generated}/${stem}.h" "${generated}/${stem}.c" "${generated}/${stem}.S"
        COMMAND "${python}" "${POCKETJS_PACKAGE_EMBED_TOOL}"
            "--package=${package}" "--host-profile=${profile}"
            "--name=${name}" "--output-dir=${generated}"
        DEPENDS "${package}" "${profile}" "${POCKETJS_PACKAGE_EMBED_TOOL}"
        VERBATIM
    )
    string(MAKE_C_IDENTIFIER "${target}_${name}" generated_suffix)
    add_custom_target("pocketjs_${generated_suffix}_package"
        DEPENDS "${generated}/${stem}.h" "${generated}/${stem}.c" "${generated}/${stem}.S")
    add_dependencies("${target}" "pocketjs_${generated_suffix}_package")
    target_sources("${target}" PRIVATE "${generated}/${stem}.c" "${generated}/${stem}.S")
    target_include_directories("${target}" PUBLIC "${generated}")
endfunction()

function(pocketjs_embed_package)
    cmake_parse_arguments(APP "" "TARGET;NAME;PACKAGE;HOST_PROFILE" "" ${ARGN})
    foreach(required TARGET NAME PACKAGE HOST_PROFILE)
        if(NOT APP_${required})
            message(FATAL_ERROR "pocketjs_embed_package requires ${required}")
        endif()
    endforeach()
    get_filename_component(package "${APP_PACKAGE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(profile "${APP_HOST_PROFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${package}")
        message(FATAL_ERROR "PocketJS package does not exist: ${package}")
    endif()
    if(NOT EXISTS "${profile}")
        message(FATAL_ERROR "PocketJS host profile does not exist: ${profile}")
    endif()
    _pocketjs_package_attach("${APP_TARGET}" "${APP_NAME}" "${package}" "${profile}")
endfunction()

function(pocketjs_compile_app)
    cmake_parse_arguments(APP "" "TARGET;NAME;MANIFEST;HOST_PROFILE;PROJECT_ROOT" "" ${ARGN})
    foreach(required TARGET NAME MANIFEST HOST_PROFILE)
        if(NOT APP_${required})
            message(FATAL_ERROR "pocketjs_compile_app requires ${required}")
        endif()
    endforeach()
    find_program(POCKETJS_CLI NAMES pocket)
    if(NOT POCKETJS_CLI)
        message(FATAL_ERROR
            "pocketjs_compile_app requires the PocketJS CLI in PATH; use pocketjs_embed_package for a prebuilt app")
    endif()
    get_filename_component(manifest "${APP_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(profile "${APP_HOST_PROFILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(APP_PROJECT_ROOT)
        get_filename_component(project_root "${APP_PROJECT_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        get_filename_component(project_root "${manifest}" DIRECTORY)
    endif()
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/pocketjs/${APP_NAME}")
    set(package "${generated}/${APP_NAME}.pocket")
    # Existing project files become build dependencies. Avoid CONFIGURE_DEPENDS:
    # IDF writes JSON under build directories while Ninja is running, which would
    # otherwise force a CMake regeneration loop.
    file(GLOB_RECURSE pocketjs_app_sources
        "${project_root}/*.ts"
        "${project_root}/*.tsx"
        "${project_root}/*.vue"
        "${project_root}/*.json")
    list(FILTER pocketjs_app_sources EXCLUDE REGEX
        "/(node_modules|build[^/]*|dist|managed_components|[.]git|[.]pocket|[.]pocket-build)/")
    add_custom_command(
        OUTPUT "${package}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${generated}"
        COMMAND "${POCKETJS_CLI}" build
            --manifest "${manifest}"
            --host-profile "${profile}"
            --project-root "${project_root}"
            --outdir "${generated}"
            --plan-dir "${generated}/plans"
            --output "${package}"
        DEPENDS "${manifest}" "${profile}" ${pocketjs_app_sources}
        VERBATIM
    )
    _pocketjs_package_attach("${APP_TARGET}" "${APP_NAME}" "${package}" "${profile}")
endfunction()
