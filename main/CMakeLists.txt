idf_component_register(SRCS "app_main.c"
                            "rl78_prog.c"
                            "image_parser.c"
                            "storage.c"
                            "wifi_ap.c"
                            "http_server.c"
                            "prog_job.c"
                            "drv_st7789v.c"
                            "drv_touch_key.c"
                            "drv_beep.c"
                            "ui_main.c"
                            "fonts/lv_font_prog_cjk_16.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "index.html"
                       REQUIRES driver esp_wifi esp_netif esp_http_server nvs_flash
                                esp_timer esp_lcd esp_lcd_st7789 app_update esp_app_format
                                joltwallet__littlefs lvgl__lvgl)
