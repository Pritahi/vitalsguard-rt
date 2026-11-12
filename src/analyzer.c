/*
 * analyzer.c - The real-time core.
 *
 * A SCHED_FIFO thread woken every 10 ms by a CLOCK_MONOTONIC periodic
 * timer pulse (QNX native, no polling). Each wake it:
 *   1. measures its own wake-up latency (expected vs actual)
 *   2. samples the physiological waveforms (simulated front-end)
 *   3. detects QRS complexes (adaptive threshold + 250 ms refractory)
 *   4. updates HR / SpO2 / RR estimates
 *   5. evaluates alarm conditions and notifies the alarm manager via
 *      QNX pulses (native microkernel message passing)
 *   6. heartbeats the watchdog path so a stalled analyzer is caught
 *
 * No dynamic allocation, no blocking I/O, no page-faultable logging on
 * the real-time path: those are the classic sources of jitter this
 * project exists to eliminate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "vitals.h"

#define ANALYSIS_PERIOD_NS  (10 * 1000 * 1000)   /* 10 ms / 100 Hz */
#define QRS_REFRACTORY_NS   (250 * 1000 * 1000)  /* 250 ms */
#define ASYSTOLE_NS         (4200LL * 1000 * 1000)
#define APNEA_NS            (15000LL * 1000 * 1000)
#define HR_BRADY_X100       4000
#define HR_TACHY_X100       13000
#define SPO2_LOW_X100       9000

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

typedef struct {
    int      chid;        /* private pulse channel */
    int      coid;        /* self-connection */
    int      alarm_coid;  /* connection to alarm manager */
    timer_t  timerid;
    int      pulse_code_rt;
} rt_ctx_t;

/* QRS detector: adaptive threshold peak picker */
typedef struct {
    double   thresh;
    double   noise;
    double   signal;
    uint64_t last_qrs_ns;
    uint64_t prev_rr_ns[8];
    int      rr_idx, rr_cnt;
    int      above;
} qrs_t;

static int qrs_update(qrs_t *q, double v, uint64_t t_ns, double *rr_mean_s)
{
    int beat = 0;
    if (v > q->thresh && !q->above) {
        if (t_ns - q->last_qrs_ns > QRS_REFRACTORY_NS && q->last_qrs_ns != 0) {
            uint64_t rr = t_ns - q->last_qrs_ns;
            q->prev_rr_ns[q->rr_idx] = rr;
            q->rr_idx = (q->rr_idx + 1) % 8;
            if (q->rr_cnt < 8) q->rr_cnt++;
            beat = 1;
        }
        q->last_qrs_ns = t_ns;
        q->signal = 0.85 * q->signal + 0.15 * v;
        q->above = 1;
    } else if (v < q->thresh * 0.5) {
        q->above = 0;
        q->noise = 0.9 * q->noise + 0.1 * fabs(v);
    }
    q->thresh = q->noise + 0.4 * (q->signal - q->noise);
    if (q->rr_cnt > 0) {
        double s = 0;
        for (int i = 0; i < q->rr_cnt; i++) s += q->prev_rr_ns[i];
        *rr_mean_s = s / q->rr_cnt / 1e9;
    }
    return beat;
}

void *analyzer_main(void *arg)
{
    rt_ctx_t ctx;
    (void)arg;

    /* --- raise to real-time priority: SCHED_FIFO, prio 60 --- */
    struct sched_param sp = { .sched_priority = 60 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != EOK)
        perror("pthread_setschedparam (need PROCMGR_AID_PRIORITY? running as root in QEMU image is fine)");

    /* --- private channel for timer pulses --- */
    ctx.chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY);
    if (ctx.chid == -1) { perror("ChannelCreate"); return NULL; }
    ctx.coid = ConnectAttach(0, 0, ctx.chid, _NTO_SIDE_CHANNEL, 0);
    if (ctx.coid == -1) { perror("ConnectAttach"); return NULL; }

    /* --- connect to the alarm manager by name (QNX name service) --- */
    ctx.alarm_coid = -1;
    for (int i = 0; i < 50 && ctx.alarm_coid == -1; i++) {
        ctx.alarm_coid = name_open(ALARM_CHANNEL_NAME, 0);
        if (ctx.alarm_coid == -1) usleep(100000);
    }
    if (ctx.alarm_coid == -1) { perror("name_open"); return NULL; }

    /* --- periodic CLOCK_MONOTONIC timer, delivered as a pulse --- */
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, ctx.coid, SI_MINAVAIL, _PULSE_CODE_MINAVAIL + 1, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &ctx.timerid) == -1) {
        perror("timer_create"); return NULL;
    }
    struct itimerspec it;
    it.it_value.tv_sec = 0; it.it_value.tv_nsec = ANALYSIS_PERIOD_NS;
    it.it_interval.tv_sec = 0; it.it_interval.tv_nsec = ANALYSIS_PERIOD_NS;
    if (timer_settime(ctx.timerid, 0, &it, NULL) == -1) {
        perror("timer_settime"); return NULL;
    }

    qrs_t qrs = { .thresh = 0.3, .noise = 0.01, .signal = 1.0 };
    uint64_t expected = now_ns() + ANALYSIS_PERIOD_NS;
    uint64_t last_qrs_ns = 0, last_breath_ns = now_ns(), start_ns = now_ns();
    double prev_resp = 0.0;
    double t_sim = 0.0;
    int beat_wd = 0;

    struct _pulse pulse;
    for (;;) {
        /* block until the next timer pulse (microkernel delivers it) */
        if (MsgReceivePulse(ctx.chid, &pulse, sizeof pulse, NULL) == -1)
            continue;

        uint64_t now = now_ns();

        /* 1. self-measured wake latency */
        uint64_t lat = (now > expected) ? now - expected : 0;
        atomic_store(&g_vitals.latency_last_us, lat / 1000);
        uint64_t w = atomic_load(&g_vitals.latency_worst_us);
        while (lat / 1000 > w &&
               !atomic_compare_exchange_weak(&g_vitals.latency_worst_us, &w, lat / 1000))
            ;
        expected += ANALYSIS_PERIOD_NS;
        if (expected < now) expected = now + ANALYSIS_PERIOD_NS;

        /* 2. sample waveforms for the active scenario */
        int sc = atomic_load(&g_vitals.scenario);
        t_sim += ANALYSIS_PERIOD_NS / 1e9;
        double ecg  = ecg_sample(t_sim, sc);
        double spo2 = spo2_sample(t_sim, sc);
        double resp = resp_sample(t_sim, sc);
        atomic_store(&g_vitals.ecg_x1000, (int32_t)(ecg * 1000));
        atomic_store(&g_vitals.spo2_x100, (int32_t)(spo2 * 100));

        /* 3. QRS / HR */
        double rr_mean = 0;
        if (qrs_update(&qrs, ecg, now, &rr_mean)) {
            last_qrs_ns = now;
            atomic_fetch_add(&g_vitals.beats, 1);
        }
        if (rr_mean > 0)
            atomic_store(&g_vitals.hr_x100, (int32_t)(60.0 / rr_mean * 100));

        /* 4. respiration: rising zero-crossing = breath */
        if (prev_resp <= 0.0 && resp > 0.0) last_breath_ns = now;
        prev_resp = resp;
        double resp_period = (now - last_breath_ns) / 1e9;
        if (resp_period < 30.0 && resp_period > 0.5)
            atomic_store(&g_vitals.rr_x100, (int32_t)(60.0 / resp_period * 100));

        /* 5. alarm evaluation -> native pulses to the alarm manager */
        int32_t hr = atomic_load(&g_vitals.hr_x100);
        if (now - last_qrs_ns > ASYSTOLE_NS && now - start_ns > ASYSTOLE_NS)
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ALARM_RAISE, COND_ASYSTOLE);
        else if (hr > 0 && hr < HR_BRADY_X100)
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ALARM_RAISE, COND_BRADYCARDIA);
        else if (hr > HR_TACHY_X100)
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ALARM_RAISE, COND_TACHYCARDIA);
        if (atomic_load(&g_vitals.spo2_x100) < SPO2_LOW_X100)
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ALARM_RAISE, COND_SPO2_LOW);
        if (now - last_breath_ns > APNEA_NS)
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ALARM_RAISE, COND_APNEA);

        /* 6. watchdog heartbeat (every ~500 ms) */
        if (++beat_wd >= 50) {
            beat_wd = 0;
            MsgSendPulse(ctx.alarm_coid, -1, PULSE_ANALYZER_BEAT, 0);
        }
    }
    return NULL;
}
