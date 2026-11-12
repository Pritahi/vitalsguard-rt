/*
 * vitals.h - Shared types and constants for VitalsGuard-RT
 *
 * VitalsGuard-RT: a deterministic real-time patient-vitals alarm engine
 * for the QNX 8.0 (Neutrino) RTOS. Built for the QNX Global Hackathon.
 *
 * Architecture (all POSIX + QNX Neutrino native IPC):
 *   - vitalsd thread:   resource manager + physiological waveform generators
 *                       (ECG 100 Hz, SpO2 25 Hz, RESP 25 Hz) mounted at
 *                       /dev/vitals/ (ecg, spo2, resp, alarm, scenario)
 *   - analyzer thread:  SCHED_FIFO real-time analysis (heart rate, SpO2
 *                       drift, apnea), self-measured wake-up latency
 *   - alarm manager:    pulse-driven alarm state machine with IEC 60601-1-8
 *                       style priorities, latching and escalation
 *   - watchdog:         CLOCK_MONOTONIC deadline supervision of the analyzer
 *   - httpd thread:     live JSON + strip-chart dashboard over TCP
 *   - loadhog:          best-effort CPU stressor to prove determinism
 *                       under 100% CPU load
 */
#ifndef VITALS_H
#define VITALS_H

#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/* Alarm severities (IEC 60601-1-8 inspired) */
typedef enum {
    ALARM_NONE = 0,
    ALARM_LOW,          /* advisory */
    ALARM_MEDIUM,       /* warning  */
    ALARM_HIGH          /* crisis   */
} alarm_sev_t;

/* Alarm conditions */
typedef enum {
    COND_NONE = 0,
    COND_BRADYCARDIA,   /* HR < 40           */
    COND_TACHYCARDIA,   /* HR > 130          */
    COND_ASYSTOLE,      /* no QRS for 4.2 s  */
    COND_SPO2_LOW,      /* SpO2 < 90 %       */
    COND_APNEA,         /* no breath 15 s    */
    COND_WATCHDOG       /* analyzer stalled  */
} alarm_cond_t;

/* Pulse codes sent analyzer -> alarm manager (QNX native message passing) */
#define PULSE_ALARM_RAISE   0x101
#define PULSE_ALARM_CLEAR   0x102
#define PULSE_WATCHDOG_TRIP 0x103
#define PULSE_ANALYZER_BEAT 0x104   /* watchdog heartbeat */

typedef struct alarm_pulse {
    uint16_t      code;
    alarm_cond_t  cond;
    alarm_sev_t   sev;
    int32_t       value_x100;   /* e.g. HR * 100 */
    uint64_t      ts_us;        /* CLOCK_MONOTONIC microseconds */
} alarm_pulse_t;

/* Live shared vitals snapshot (single writer: analyzer; readers: httpd) */
typedef struct vitals_snapshot {
    _Atomic int32_t  hr_x100;        /* heart rate * 100        */
    _Atomic int32_t  spo2_x100;      /* SpO2 * 100              */
    _Atomic int32_t  rr_x100;        /* respiration rate * 100  */
    _Atomic int32_t  alarm_sev;      /* current max severity    */
    _Atomic int32_t  alarm_cond;     /* current condition       */
    _Atomic uint64_t latency_worst_us;   /* worst analyzer jitter */
    _Atomic uint64_t latency_last_us;    /* last wakeup latency   */
    _Atomic uint64_t beats;          /* total QRS count         */
    _Atomic uint64_t uptime_s;
    _Atomic int32_t  loadhog_on;
    _Atomic int32_t  scenario;      /* active demo scenario   */
    _Atomic int32_t  ecg_x1000;      /* latest ECG sample *1000 */
} vitals_snapshot_t;

extern vitals_snapshot_t g_vitals;

/* Waveform sample generators (vitalsd.c) */
double ecg_sample(double t_s, int scenario);
double spo2_sample(double t_s, int scenario);
double resp_sample(double t_s, int scenario);

/* Scenarios */
enum { SCENARIO_NORMAL = 0, SCENARIO_TACHY, SCENARIO_BRADY,
       SCENARIO_ASYSTOLE, SCENARIO_HYPOXIA, SCENARIO_APNEA };

/* Alarm manager channel name */
#define ALARM_CHANNEL_NAME "vitalsguard/alarms"

#endif /* VITALS_H */
