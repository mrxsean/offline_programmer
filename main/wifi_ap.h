/* WiFi SoftAP for the web management interface. */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the SoftAP with credentials from Kconfig. Logs the AP IP address. */
esp_err_t wifi_ap_init(void);
const char *wifi_ap_ssid(void);

#ifdef __cplusplus
}
#endif
