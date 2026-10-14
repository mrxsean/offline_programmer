/*
 * I2C touch keypad (6 keys, CAP1298-compatible chip @ 0x28, INT on GPIO).
 * Key events are delivered through the que_key queue (key_msg_t).
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_BUTTON_NUM 6

/* Key codes (combinable by OR for combos) */
#define KEY_DOWN   0x02
#define KEY_ONOFF  0x04
#define KEY_MENU   0x10
#define KEY_UP     0x20
#define KEY_OK     0x40
#define KEY_MODE   0x80

typedef enum {
    KEY_EVT_PRESS,      /* key down (sent immediately, combos included) */
    KEY_EVT_RELEASE,    /* all keys released (only if no long-press fired) */
    KEY_EVT_LONG,       /* long press (1000 ms), fired once */
    KEY_EVT_REPEAT,     /* auto-repeat while holding UP/DOWN (100 ms) */
} key_evt_t;

typedef struct {
    uint16_t  code;     /* KEY_* */
    key_evt_t evt;
} key_msg_t;

/* Key message queue (read task -> UI), elements are key_msg_t */
extern QueueHandle_t que_key;

void touch_key_start(void);

#ifdef __cplusplus
}
#endif
