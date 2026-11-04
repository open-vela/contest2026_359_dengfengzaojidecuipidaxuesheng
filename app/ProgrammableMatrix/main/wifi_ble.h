#pragma once
#include <stdbool.h>
void wifi_ble_start(void);
bool wifi_is_ready(void);
bool wifi_has_credentials(void);
void wifi_ble_button_short_press(void);
void wifi_ble_forget_wifi(void);
