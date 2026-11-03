idf_component_register(
    SRCS "main.c"
         "cs_app.c"
         "cs_console.c"
         "cs_market_model.c"
         "cs_market_store.c"
         "cs_net.c"
         "cs_ui.c"
         "fonts/cs_font_16.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES bsp console esp_event esp_hw_support esp_netif esp_timer esp_wifi
             esp_http_client esp-tls mbedtls json lwip nvs_flash
)
