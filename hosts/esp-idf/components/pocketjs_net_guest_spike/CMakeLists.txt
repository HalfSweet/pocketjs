idf_component_register(
    SRCS "src/net_guest_spike.c"
    INCLUDE_DIRS "include"
    REQUIRES pocketjs_esp_guest pocketjs_net_esp_http quickjs-ng
    PRIV_REQUIRES esp_timer freertos log
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror
)
