/*
 * main.c - VitalsGuard-RT orchestrator.
 *
 * Threads (in priority order):
 *   analyzer  SCHED_FIFO prio 60  - the real-time safety path
 *   alarmd    default prio        - alarm state machine (pulse server)
 *   vitalsd   default prio        - /dev/vitals/ resource manager
 *   httpd     prio 10             - dashboard (deliberately deprioritized)
 *   loadhog   prio 1              - CPU stressor for the load demo
 *
 * Usage: vitalsguard [-p http_port] [-s scenario]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "vitals.h"

int g_no_rt = 0;
int g_no_alarm_tx = 0;

void *analyzer_main(void *);
void *alarmd_main(void *);
void *httpd_main(void *);
void *loadhog_main(void *);
int   vitalsd_start(void);

int main(int argc, char **argv)
{
    int http_port = 8080;
    int opt;
    while ((opt = getopt(argc, argv, "p:s:ANh")) != -1) {
        switch (opt) {
        case 'p': http_port = atoi(optarg); break;
        case 's': atomic_store(&g_vitals.scenario, atoi(optarg)); break;
        case 'A': g_no_rt = 1; break;
        case 'N': g_no_alarm_tx = 1; break;
        default:
            fprintf(stderr, "usage: %s [-p http_port] [-s scenario 0-5]\n", argv[0]);
            return 1;
        }
    }

    printf("VitalsGuard-RT - deterministic vitals alarm engine on QNX 8.0\n");
    printf("  /dev/vitals/ resource manager, SCHED_FIFO analyzer,\n");
    printf("  pulse-driven alarm manager, watchdog, live dashboard :%d\n",
           http_port);
    fflush(stdout);

    pthread_t t_alarm, t_http, t_hog, t_an;

    /* alarm manager first: it owns the channel the analyzer connects to */
    pthread_create(&t_alarm, NULL, alarmd_main, NULL);
    pthread_create(&t_http,  NULL, httpd_main, (void *)(long)http_port);
    pthread_create(&t_hog,   NULL, loadhog_main, NULL);

    usleep(200000);   /* let the alarm channel register */

    /* the real-time path runs on its own thread ... */
    pthread_create(&t_an, NULL, analyzer_main, NULL);

    /* ... while the MAIN thread runs the resource-manager thread pool:
       thread_pool_start() must block the process's first thread, otherwise
       the pool exits immediately and /dev/vitals is never serviced */
    vitalsd_start();
    return 0;
}
