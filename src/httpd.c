/*
 * httpd.c - Minimal HTTP dashboard server (thread, non-RT).
 *
 * Serves:
 *   GET /         -> live strip-chart dashboard (embedded HTML/JS)
 *   GET /vitals   -> JSON snapshot of the shared vitals state
 *
 * Runs at low priority: the dashboard can never starve the alarm path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "vitals.h"

static const char DASH_HTML[] =
"<!doctype html><html><head><meta charset=utf-8><title>VitalsGuard-RT</title>\n"
"<style>body{background:#0b1020;color:#dfe7ff;font-family:system-ui;margin:24px}\n"
"h1{font-size:20px}canvas{background:#111a33;border-radius:8px;display:block;margin:8px 0}\n"
".b{font-size:28px;font-weight:700;display:inline-block;margin-right:32px}\n"
"#alarm{padding:10px 16px;border-radius:8px;font-weight:700;margin:12px 0}\n"
".ok{background:#123a24}.warn{background:#5a4500}.crit{background:#6b1020;animation:bl 1s infinite}\n"
"@keyframes bl{50%{filter:brightness(1.6)}}small{color:#8fa3d9}</style></head><body>\n"
"<h1>VitalsGuard-RT - deterministic vitals alarm engine on QNX 8.0</h1>\n"
"<div><span class=b>HR <span id=hr>--</span></span><span class=b>SpO2 <span id=spo2>--</span>%</span>\n"
"<span class=b>RR <span id=rr>--</span></span><span class=b>worst wake latency <span id=lat>--</span> us</span></div>\n"
"<div id=alarm class=ok>NO ALARM</div>\n"
"<canvas id=c width=900 height=260></canvas>\n"
"<small>ECG strip (100 Hz) - served live from a QNX Neutrino guest</small>\n"
"<script>let D=[];async function tick(){try{let v=await(await fetch('/vitals')).json();\n"
"hr.textContent=(v.hr_x100/100).toFixed(0);spo2.textContent=(v.spo2_x100/100).toFixed(1);\n"
"rr.textContent=(v.rr_x100/100).toFixed(0);lat.textContent=v.latency_worst_us;\n"
"let a=document.getElementById('alarm');\n"
"if(v.alarm_sev>=3){a.className='crit';a.textContent='HIGH ALARM - '+v.cond_name}\n"
"else if(v.alarm_sev>=2){a.className='warn';a.textContent='MEDIUM ALARM - '+v.cond_name}\n"
"else{a.className='ok';a.textContent='NO ALARM'}\n"
"D.push(v.ecg_x1000/1000);if(D.length>900)D.shift();draw();}catch(e){}setTimeout(tick,100)}\n"
"function draw(){let c=document.getElementById('c'),x=c.getContext('2d');\n"
"x.clearRect(0,0,900,260);x.strokeStyle='#39d98a';x.lineWidth=2;x.beginPath();\n"
"D.forEach((v,i)=>{let y=130-v*100;i?x.lineTo(i,y):x.moveTo(i,y)});x.stroke()}tick();</script>\n"
"</body></html>";

static void send_all(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, buf, n, 0);
        if (w <= 0) return;
        buf += w; n -= (size_t)w;
    }
}

static const char *cond_names[] = {
    "NONE","BRADYCARDIA","TACHYCARDIA","ASYSTOLE","SPO2_LOW","APNEA","WATCHDOG"
};

static void handle_client(int fd)
{
    char req[1024];
    ssize_t r = recv(fd, req, sizeof req - 1, 0);
    if (r <= 0) return;
    req[r] = '\0';

    if (strncmp(req, "GET /vitals", 11) == 0) {
        int cond = atomic_load(&g_vitals.alarm_cond);
        char body[640];
        int n = snprintf(body, sizeof body,
            "{\"hr_x100\":%d,\"spo2_x100\":%d,\"rr_x100\":%d,"
            "\"alarm_sev\":%d,\"alarm_cond\":%d,\"cond_name\":\"%s\","
            "\"latency_worst_us\":%llu,\"latency_last_us\":%llu,"
            "\"beats\":%llu,\"ecg_x1000\":%d,\"scenario\":%d,"
            "\"loadhog\":%d,\"uptime_s\":%llu}",
            atomic_load(&g_vitals.hr_x100), atomic_load(&g_vitals.spo2_x100),
            atomic_load(&g_vitals.rr_x100), atomic_load(&g_vitals.alarm_sev),
            cond, cond_names[cond >= 0 && cond <= 6 ? cond : 0],
            (unsigned long long)atomic_load(&g_vitals.latency_worst_us),
            (unsigned long long)atomic_load(&g_vitals.latency_last_us),
            (unsigned long long)atomic_load(&g_vitals.beats),
            atomic_load(&g_vitals.ecg_x1000), atomic_load(&g_vitals.scenario),
            atomic_load(&g_vitals.loadhog_on),
            (unsigned long long)atomic_load(&g_vitals.uptime_s));
        char hdr[128];
        int h = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
        send_all(fd, hdr, h);
        send_all(fd, body, n);
    } else {
        char hdr[160];
        int h = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            sizeof(DASH_HTML) - 1);
        send_all(fd, hdr, h);
        send_all(fd, DASH_HTML, sizeof(DASH_HTML) - 1);
    }
}

void *httpd_main(void *arg)
{
    int port = (int)(long)arg;

    /* deliberately low priority: monitoring UI must never preempt alarms */
    struct sched_param sp = { .sched_priority = 10 };
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return NULL; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind"); return NULL;
    }
    if (listen(s, 4) < 0) { perror("listen"); return NULL; }
    printf("[httpd] dashboard on port %d\n", port);
    fflush(stdout);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c >= 0) { handle_client(c); close(c); }
    }
    return NULL;
}
