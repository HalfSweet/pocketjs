set(_pocketjs_tls_smoke
    "${CMAKE_CURRENT_LIST_DIR}/../pocketjs_net_formal_tls_smoke_artifact")

idf_component_register(
  SRCS
    "generated/formal_tls_smoke_metadata.c"
    "${_pocketjs_tls_smoke}/src/formal_tls_smoke_ipv4.c"
    "${_pocketjs_tls_smoke}/src/formal_tls_smoke_report.c"
    "${_pocketjs_tls_smoke}/src/formal_tls_smoke_runner.c"
  INCLUDE_DIRS
    "${_pocketjs_tls_smoke}/include"
    "../../../../contracts/spec/network/generated"
  PRIV_INCLUDE_DIRS
    "${_pocketjs_tls_smoke}/private"
  REQUIRES
    pocketjs_esp_guest
    pocketjs_net_esp_runtime
    esp_timer
    lwip
    quickjs-ng
)

target_add_binary_data(
  ${COMPONENT_LIB}
  "generated/factory.js.bin"
  BINARY
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
