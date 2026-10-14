/* Passive buzzer on GPIO37 (LEDC PWM). Non-blocking pattern playback. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BEEP_PAT_KEY,      /* key click: one short beep */
    BEEP_PAT_BOOT,     /* power-on: one medium beep */
    BEEP_PAT_START,    /* job started: two short beeps */
    BEEP_PAT_DONE,     /* success: three short beeps */
    BEEP_PAT_FAIL,     /* failure: one long low beep */
    BEEP_PAT_ERROR,    /* invalid operation: two short low beeps */
} beep_pattern_t;

/* Initialize PWM + playback task. Call once at boot. */
void beep_init(void);

/* Queue a pattern for playback (drops silently when the queue is full). */
void beep_play(beep_pattern_t pat);

#ifdef __cplusplus
}
#endif
