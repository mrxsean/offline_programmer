idf_component_register(SRCS "app_main.c"
                            "rl78_prog.c"
                            "image_parser.c"
                            "storage.c"
                            "wifi_ap.c"
                            "http_server.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html"
                       REQUIRES driver esp_wifi esp_netif esp_http_server nvs_flash joltwallet__littlefs)
