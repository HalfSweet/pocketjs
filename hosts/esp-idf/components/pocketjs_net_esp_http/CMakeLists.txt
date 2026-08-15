idf_component_register(
    SRCS "src/esp_http_client_backend.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos
    PRIV_REQUIRES esp_http_client esp-tls heap log mbedtls
)
