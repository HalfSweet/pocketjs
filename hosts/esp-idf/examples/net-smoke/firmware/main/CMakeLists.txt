# The smoke bundle is built from ../../app.ts by the PocketJS compiler (bun) into
# the build directory and embedded as text. Set POCKETJS_ROOT / BUN when the
# defaults do not match the checkout.
if(NOT DEFINED POCKETJS_ROOT)
  get_filename_component(POCKETJS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../.." ABSOLUTE)
endif()
if(NOT DEFINED BUN)
  find_program(BUN bun HINTS "$ENV{HOME}/.bun/bin" "/opt/homebrew/bin" "/usr/local/bin")
endif()
set(SMOKE_APP "${POCKETJS_ROOT}/hosts/esp-idf/examples/net-smoke/app.ts")
set(SMOKE_OUT "${CMAKE_BINARY_DIR}/pocketjs-app")
set(SMOKE_JS "${SMOKE_OUT}/app.js")

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS ""
    REQUIRES pocketjs_esp_host pocketjs_board pocketjs_net_core
    PRIV_REQUIRES esp_timer heap
    EMBED_TXTFILES "${SMOKE_JS}")

add_custom_command(
    OUTPUT "${SMOKE_JS}"
    COMMAND "${BUN}" "${POCKETJS_ROOT}/tools/build.ts" "${SMOKE_APP}" "--outdir=${SMOKE_OUT}"
    WORKING_DIRECTORY "${POCKETJS_ROOT}"
    DEPENDS "${SMOKE_APP}"
    COMMENT "PocketJS: bundling ${SMOKE_APP}"
    VERBATIM)
add_custom_target(pocketjs_smoke_bundle DEPENDS "${SMOKE_JS}")
add_dependencies(${COMPONENT_LIB} pocketjs_smoke_bundle)
