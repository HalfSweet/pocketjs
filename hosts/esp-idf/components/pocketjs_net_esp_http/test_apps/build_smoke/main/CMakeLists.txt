idf_component_register(
    SRCS "build_smoke.c"
    INCLUDE_DIRS "."
    REQUIRES pocketjs_net_esp_http
    PRIV_REQUIRES esp-tls mbedtls
)

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE POCKETJS_NET_ESP_HTTP_TEST_INTERNALS=1
)
