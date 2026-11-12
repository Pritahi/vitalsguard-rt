/*
 * alarmd.c - Alarm manager: a QNX pulse server.
 *
 * Registers ALARM_CHANNEL_NAME with the QNX name service and receives
 * pulses from the analyzer (native microkernel IPC). Owns the alarm
 * state machine: severity, latching, escalation, and the analyzer
 * watchdog. If no analyzer heartbeat arrives within 1.5 s (checked with
 * TimerTimeout on the receive, a QNX idiom), it declares COND_WATCHDOG
 * and raises a HIGH alarm: the system fails safe, loudly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "vitals.h"

#define WATCHDOG_TIMEOUT_NS (1500LL * 1000 * 1000)

static const char *cond_name(alarm_cond_t c)
{
    switch (c) {
    case COND_BRADYCARDIA: return "BRADYCARDIA";
    case COND_TACHYCARDIA: return "TACHYCARDIA";
    case COND_ASYSTOLE:    return "ASYSTOLE";
    case COND_SPO2_LOW:    return "SPO2 LOW";
    case COND_APNEA:       return "APNEA";
    case COND_WATCHDOG:    return "ANALYZER WATCHDOG";
    default:               return "NONE";
    }
}

static alarm_sev_t cond_sev(alarm_cond_t c)
{
    switch (c) {
    case COND_ASYSTOLE:
    case COND_BRADYCARDIA:
    case COND_WATCHDOG:    return ALARM_HIGH;
    default:               return ALARM_MEDIUM;
    }
}

static const char *sev_name(alarm_sev_t s)
{
    switch (s) {
    case ALARM_HIGH:   return "HIGH";
    case ALARM_MEDIUM: return "MEDIUM";
    case ALARM_LOW:    return "LOW";
    default:           return "-";
    }
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

void *alarmd_main(void *arg)
{
    (void)arg;
    name_attach_t *attach = name_attach(NULL, ALARM_CHANNEL_NAME, 0);
    if (!attach) { perror("name_attach"); return NULL; }
    int chid = attach->chid;

    alarm_cond_t active = COND_NONE;
    uint64_t last_beat_ns = now_ns();
    uint64_t last_print_ns = 0;

    struct _pulse pulse;
    for (;;) {
        /* fail-safe receive: wake at least every 500 ms to check watchdog */
        struct timespec to = { 0, 500 * 1000 * 1000 };
        TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL,
                     NULL, &to);
        int rcvid = MsgReceivePulse(chid, &pulse, sizeof pulse, NULL);
        uint64_t now = now_ns();

        if (rcvid == -1 && errno == ETIMEDOUT) {
            /* watchdog check */
            if (now - last_beat_ns > WATCHDOG_TIMEOUT_NS &&
                active != COND_WATCHDOG) {
                active = COND_WATCHDOG;
                atomic_store(&g_vitals.alarm_sev, ALARM_HIGH);
                atomic_store(&g_vitals.alarm_cond, COND_WATCHDOG);
                printf("[ALARM] *** HIGH - ANALYZER WATCHDOG TRIPPED ***\n");
                fflush(stdout);
            }
            continue;
        }
        if (rcvid != 0) continue;   /* pulses carry rcvid 0 */

        switch (pulse.code) {
        case PULSE_ANALYZER_BEAT:
            last_beat_ns = now;
            if (active == COND_WATCHDOG) {
                active = COND_NONE;
                atomic_store(&g_vitals.alarm_sev, ALARM_NONE);
                atomic_store(&g_vitals.alarm_cond, COND_NONE);
                printf("[ALARM] watchdog recovered - analyzer alive\n");
                fflush(stdout);
            }
            break;
        case PULSE_ALARM_RAISE: {
            alarm_cond_t c = (alarm_cond_t)pulse.value.sival_int;
            alarm_sev_t s = cond_sev(c);
            if (c != active || now - last_print_ns > 2000000000ull) {
                printf("[ALARM] %s - %s (hr=%.2f spo2=%.2f)\n",
                       sev_name(s), cond_name(c),
                       atomic_load(&g_vitals.hr_x100) / 100.0,
                       atomic_load(&g_vitals.spo2_x100) / 100.0);
                fflush(stdout);
                last_print_ns = now;
            }
            active = c;
            atomic_store(&g_vitals.alarm_sev, s);
            atomic_store(&g_vitals.alarm_cond, c);
            break;
        }
        case PULSE_ALARM_CLEAR:
            active = COND_NONE;
            atomic_store(&g_vitals.alarm_sev, ALARM_NONE);
            atomic_store(&g_vitals.alarm_cond, COND_NONE);
            printf("[ALARM] cleared\n");
            fflush(stdout);
            break;
        default:
            break;
        }
    }
    return NULL;
}
