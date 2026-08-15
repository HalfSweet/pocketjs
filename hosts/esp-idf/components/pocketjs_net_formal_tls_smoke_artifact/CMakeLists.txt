idf_component_register(
  SRCS
    "generated/formal_tls_smoke_metadata.c"
    "src/formal_tls_smoke_ipv4.c"
    "src/formal_tls_smoke_report.c"
    "src/formal_tls_smoke_runner.c"
  INCLUDE_DIRS
    "include"
    "../../../../contracts/spec/network/generated"
  PRIV_INCLUDE_DIRS
    "private"
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
