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

void *analyzer_main(void *);
void *alarmd_main(void *);
void *httpd_main(void *);
void *loadhog_main(void *);
int   vitalsd_start(void);
static void *vitalsd_thread(void *a) { (void)a; vitalsd_start(); return NULL; }

int main(int argc, char **argv)
{
    int http_port = 8080;
    int opt;
    while ((opt = getopt(argc, argv, "p:s:h")) != -1) {
        switch (opt) {
        case 'p': http_port = atoi(optarg); break;
        case 's': atomic_store(&g_vitals.scenario, atoi(optarg)); break;
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

    pthread_t t_alarm, t_http, t_hog, t_resmgr, t_an;

    /* alarm manager first: it owns the channel the analyzer connects to */
    pthread_create(&t_alarm, NULL, alarmd_main, NULL);
    pthread_create(&t_http,  NULL, httpd_main, (void *)(long)http_port);
    pthread_create(&t_hog,   NULL, loadhog_main, NULL);

    /* vitalsd_start() blocks in the resmgr thread pool: run it detached */
    pthread_create(&t_resmgr, NULL, vitalsd_thread, NULL);
    usleep(200000);   /* let /dev/vitals mount */

    /* the real-time path starts last, once every client-facing piece is up */
    pthread_create(&t_an, NULL, analyzer_main, NULL);

    pthread_join(t_an, NULL);
    return 0;
}
