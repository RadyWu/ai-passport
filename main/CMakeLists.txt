idf_component_register(
    SRCS "main.c"
         "app_article.c"
         "app_store.c"
         "app_net.c"
         "app_deepseek.c"
         "app_sleep.c"
         "app_ui.c"
         "../assets/fonts/font_toilet_14.c"
         "../assets/fonts/font_toilet_20.c"
    INCLUDE_DIRS "."
                 "../assets/fonts"
    REQUIRES bsp
             esp_event
             esp_http_client
             esp-tls
             esp_http_server
             esp_hw_support
             esp_netif
             esp_timer
             esp_wifi
             nvs_flash
             mbedtls
             dns_server
)

# Host-test-only sources (demo_*.c) remain in the tree for ./tools/validate.sh --static
# but are not linked into this derivative application firmware.
