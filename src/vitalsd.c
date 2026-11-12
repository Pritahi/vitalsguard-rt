/*
 * vitalsd.c - QNX resource manager exposing the live vitals state.
 *
 * Mounts /dev/vitals/{ecg,spo2,resp,alarm,scenario}.
 *   read  /dev/vitals/ecg|spo2|resp  -> latest values
 *   read  /dev/vitals/alarm          -> alarm + latency status line
 *   write /dev/vitals/alarm          -> acknowledge active alarm
 *   write /dev/vitals/scenario       -> switch demo scenario (0-5)
 *
 * Everything on QNX is a message; a resource manager is the idiomatic
 * way to expose device-like state as files.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#define RESMGR_OCB_T iofunc_ocb_t
#include <sys/resmgr.h>
#include <sys/neutrino.h>

#include "vitals.h"

vitals_snapshot_t g_vitals;

/* ---------- waveform synthesis (deterministic, no malloc) ---------- */

/* Synthetic ECG (piecewise Gaussian PQRST complex) */
double ecg_sample(double t_s, int scenario)
{
    double rate;
    switch (scenario) {
    case SCENARIO_TACHY:    rate = 2.4; break;   /* ~144 bpm */
    case SCENARIO_BRADY:    rate = 0.55; break;  /* ~33 bpm  */
    case SCENARIO_ASYSTOLE: rate = 0.0;  break;  /* flatline */
    default:                rate = 1.1;  break;  /* ~66 bpm  */
    }
    if (rate == 0.0)
        return 0.002 * sin(2 * M_PI * 50.0 * t_s); /* tiny mains hum only */

    double phase = fmod(t_s * rate, 1.0) * 2.0 * M_PI;
    static const struct { double th, a, w; } wv[5] = {
        { -0.90,  0.12, 0.09 },  /* P */
        { -0.12, -0.15, 0.02 },  /* Q */
        {  0.00,  1.00, 0.03 },  /* R */
        {  0.12, -0.25, 0.02 },  /* S */
        {  0.65,  0.28, 0.12 },  /* T */
    };
    double v = 0.0;
    for (int i = 0; i < 5; i++) {
        double d = phase - wv[i].th;
        v += wv[i].a * exp(-(d * d) / (2 * wv[i].w * wv[i].w));
    }
    return v;
}

double spo2_sample(double t_s, int scenario)
{
    double base = (scenario == SCENARIO_HYPOXIA) ? 86.0 : 98.0;
    return base + 0.4 * sin(2 * M_PI * 0.07 * t_s);
}

double resp_sample(double t_s, int scenario)
{
    double rate = (scenario == SCENARIO_APNEA) ? 0.0 : 0.25; /* ~15/min */
    if (rate == 0.0) return 0.0;
    return sin(2 * M_PI * rate * t_s);
}

/* ---------- resource manager ---------- */

typedef enum { DEV_ECG, DEV_SPO2, DEV_RESP, DEV_ALARM, DEV_SCENARIO } devkind_t;

typedef struct {
    iofunc_attr_t attr;
    devkind_t     kind;
} vg_attr_t;

static int vg_open(resmgr_context_t *ctp, io_open_t *msg, void *handle, void *extra)
{
    return iofunc_open_default(ctp, msg, handle, extra);
}

static int vg_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    /* reads on the /dev/vitals prefix itself (e.g. `ls /dev/vitals`) arrive
       with a directory attr that is not one of our vg_attr_t files - fail
       fast instead of mishandling it */
    if (S_ISDIR(ocb->attr->mode))
        return EISDIR;
    vg_attr_t *vattr = (vg_attr_t *)ocb->attr;
    char buf[256];
    int n;
    switch (vattr->kind) {
    case DEV_ECG:
        n = snprintf(buf, sizeof buf, "%.3f\n",
                     atomic_load(&g_vitals.ecg_x1000) / 1000.0);
        break;
    case DEV_SPO2:
        n = snprintf(buf, sizeof buf, "%.2f\n",
                     atomic_load(&g_vitals.spo2_x100) / 100.0);
        break;
    case DEV_RESP:
        n = snprintf(buf, sizeof buf, "%.2f\n",
                     atomic_load(&g_vitals.rr_x100) / 100.0);
        break;
    case DEV_SCENARIO:
        n = snprintf(buf, sizeof buf, "%d\n",
                     atomic_load(&g_vitals.scenario));
        break;
    case DEV_ALARM:
    default:
        n = snprintf(buf, sizeof buf, "sev=%d cond=%d hr=%.2f spo2=%.2f "
                     "rr=%.2f worst_latency_us=%llu last_latency_us=%llu "
                     "beats=%llu loadhog=%d\n",
                     atomic_load(&g_vitals.alarm_sev),
                     atomic_load(&g_vitals.alarm_cond),
                     atomic_load(&g_vitals.hr_x100) / 100.0,
                     atomic_load(&g_vitals.spo2_x100) / 100.0,
                     atomic_load(&g_vitals.rr_x100) / 100.0,
                     (unsigned long long)atomic_load(&g_vitals.latency_worst_us),
                     (unsigned long long)atomic_load(&g_vitals.latency_last_us),
                     (unsigned long long)atomic_load(&g_vitals.beats),
                     atomic_load(&g_vitals.loadhog_on));
        break;
    }
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    /* honour the file offset: reply once, EOF on the next read, so that
       `cat /dev/vitals/x` terminates instead of streaming forever */
    int nbytes = msg->i.nbytes;
    if ((int)ocb->offset >= n) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    if (nbytes > n - (int)ocb->offset)
        nbytes = n - (int)ocb->offset;
    SETIOV(ctp->iov, buf + ocb->offset, nbytes);
    _IO_SET_READ_NBYTES(ctp, nbytes);
    ocb->offset += nbytes;
    return _RESMGR_NPARTS(1);
}

static int vg_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    vg_attr_t *vattr = (vg_attr_t *)ocb->attr;
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;

    char *buf = NULL;
    int datalen = msg->i.nbytes;
    if (datalen > 0 && datalen < 64) {
        buf = malloc(datalen + 1);
        if (buf && resmgr_msgread(ctp, buf, datalen, sizeof(msg->i)) == datalen) {
            buf[datalen] = '\0';
            if (vattr->kind == DEV_ALARM) {
                atomic_store(&g_vitals.alarm_sev, ALARM_NONE);
                atomic_store(&g_vitals.alarm_cond, COND_NONE);
            } else if (vattr->kind == DEV_SCENARIO) {
                if (strncmp(buf, "load:", 5) == 0) {
                    /* "load:1"/"load:0" toggles the CPU stressor */
                    atomic_store(&g_vitals.loadhog_on, atoi(buf + 5) ? 1 : 0);
                } else {
                    int sc = atoi(buf);
                    if (sc >= 0 && sc <= 5)
                        atomic_store(&g_vitals.scenario, sc);
                }
            }
        }
        free(buf);
    }
    _IO_SET_WRITE_NBYTES(ctp, datalen);
    return _RESMGR_NPARTS(0);
}

int vitalsd_start(void)
{
    dispatch_t           *dpp;
    resmgr_attr_t         rattr;
    resmgr_connect_funcs_t cfuncs;
    resmgr_io_funcs_t      ifuncs;
    static vg_attr_t      attrs[5];

    /* I/O privilege is required for a resource manager */
    if (ThreadCtl(_NTO_TCTL_IO, NULL) == -1) {
        perror("ThreadCtl");
        return -1;
    }

    dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
    if (!dpp) { perror("dispatch_create_channel"); return -1; }

    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cfuncs,
                     _RESMGR_IO_NFUNCS, &ifuncs);
    cfuncs.open  = vg_open;
    ifuncs.read  = vg_read;
    ifuncs.write = vg_write;

    static const struct { const char *path; devkind_t k; } devs[] = {
        { "/dev/vitals/ecg",      DEV_ECG      },
        { "/dev/vitals/spo2",     DEV_SPO2     },
        { "/dev/vitals/resp",     DEV_RESP     },
        { "/dev/vitals/alarm",    DEV_ALARM    },
        { "/dev/vitals/scenario", DEV_SCENARIO },
    };
    for (int i = 0; i < 5; i++) {
        iofunc_attr_init(&attrs[i].attr, S_IFNAM | 0666, NULL, NULL);
        attrs[i].kind = devs[i].k;
        int id = resmgr_attach(dpp, &rattr, devs[i].path, _FTYPE_ANY, 0,
                               &cfuncs, &ifuncs, &attrs[i]);
        fprintf(stderr, "[vitalsd] attach %s -> id %d\n", devs[i].path, id);
        if (id == -1) {
            perror("resmgr_attach");
            return -1;
        }
    }

    thread_pool_attr_t tattr;
    memset(&tattr, 0, sizeof tattr);
    tattr.handle = dpp;
    tattr.context_alloc = resmgr_context_alloc;
    tattr.context_free  = resmgr_context_free;
    tattr.block_func    = dispatch_block;
    tattr.unblock_func  = dispatch_unblock;
    tattr.handler_func  = dispatch_handler;
    tattr.lo_water = 1; tattr.hi_water = 2;
    tattr.increment = 1; tattr.maximum = 2;

    thread_pool_t *tpp = thread_pool_create(&tattr, POOL_FLAG_EXIT_SELF);
    if (!tpp) { perror("thread_pool_create"); return -1; }
    thread_pool_start(tpp);   /* returns only on exit */
    return 0;
}
