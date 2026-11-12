/*
 * loadhog.c - Best-effort CPU stressor.
 *
 * Hogs the CPU at the lowest possible priority to prove that the
 * SCHED_FIFO analyzer keeps its deadlines even at 100% CPU load.
 * This is the demo: on a general-purpose OS the monitor path stutters;
 * on QNX the numbers stay flat.
 */
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <stdatomic.h>

#include "vitals.h"

void *loadhog_main(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = 1 };
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

    double x = 0.5;
    for (;;) {
        if (atomic_load(&g_vitals.loadhog_on)) {
            for (int i = 0; i < 100000; i++)
                x = sin(x) * cos(x) + 0.1;   /* burn cycles */
        } else {
            usleep(50000);
        }
    }
    return NULL;
}
