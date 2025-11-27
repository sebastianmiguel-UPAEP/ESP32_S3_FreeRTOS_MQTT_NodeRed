idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash
                    PRIV_REQUIRES mqtt nvs_flash esp_netif
                    PRIV_REQUIRES esp_adc
                    PRIV_REQUIRES driver
                    INCLUDE_DIRS ".")