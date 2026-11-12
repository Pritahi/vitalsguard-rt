---
title: VitalsGuard-RT Codelab
---

# Codelab: Zero to a deadline-guaranteed service on QNX 8.0 (QEMU, free, headless Linux)

**Audience:** developers new to QNX who want a real program running on the
QNX 8.0 RTOS with measured, bounded wake-up latency — using only free
software, no hardware, and (optionally) a headless Linux machine.

**Time:** about 45 minutes. **Cost:** $0.

You will:
1. Get QNX SDP 8.0 free via QNX Everywhere
2. Install the QNX Software Center *headlessly* on Linux (no GUI needed)
3. Boot the QNX 8.0 Quick Start Target Image in QEMU
4. Cross-compile a periodic real-time task with `qcc`
5. Measure its wake-up latency under full CPU load — and see why
   "real-time" is a promise QNX keeps

---

## Step 1 — Get your free QNX Everywhere license (5 min)

1. Create a myQNX account at https://www.qnx.com/account/login.html
   ("New Member?"). Password rule: alphanumeric only, 6-19 characters.
2. Go to https://qnx.software/en/developers/get-started/qnx-everywhere
   and complete the license form while logged in.
3. Within minutes you receive two emails: a license confirmation
   (reference serial number) and "QNX product(s) available for you" —
   the products are now deployed to your account in the QNX Software
   Center.

## Step 2 — Headless install of the QNX Software Center (10 min)

Everything below works on a plain Linux terminal — no display needed.
This is the path the QNX docs describe for build servers; it works just
as well for a laptop-free setup.

```sh
mkdir -p ~/.qnx
# Log in once to get authenticated download cookies:
curl -v --cookie-jar ~/.qnx/myqnxcookies.auth \
  --form "userlogin=YOUR_EMAIL" --form "password=YOUR_PASSWORD" \
  -X POST https://www.qnx.com/account/login.html > /dev/null

# Download the QSC Linux installer (link from
# https://www.qnx.com/download/group.html?programid=29178):
curl -L --cookie ~/.qnx/myqnxcookies.auth \
  "https://www.qnx.com/download/download/79441/qnx-setup-2.0.4-202501021438-linux.run" \
  -o qnx-setup-linux.run
chmod +x qnx-setup-linux.run

# Install (auto-accepts the EULA):
./qnx-setup-linux.run force-override disable-auto-start agree-to-license-terms ~/qnxinstall
```

## Step 3 — Activate the license and install SDP 8.0 (10 min)

Put your credentials in an options file (this keeps the password out of
the QSC log files — passing it on the command line does not):

```sh
printf -- '-myqnx.user=YOUR_EMAIL\n-myqnx.password=YOUR_PASSWORD\n' > ~/.qnx/qnx_opts.txt
chmod 600 ~/.qnx/qnx_opts.txt
cd ~/qnxinstall/qnxsoftwarecenter

./qnxsoftwarecenter_clt @~/.qnx/qnx_opts.txt -syncLicenseKeys -activateAll -listLicenseKeys
# -> Activated: QNX Software Development Platform 8.0 - Non-Commercial

./qnxsoftwarecenter_clt @~/.qnx/qnx_opts.txt -mirror -list | grep '^com.qnx.qnx800='

./qnxsoftwarecenter_clt @~/.qnx/qnx_opts.txt \
  -installBaseline com.qnx.qnx800 -destination ~/qnxinstall/qnx800

# The QEMU quick-start target image (QSTI):
./qnxsoftwarecenter_clt @~/.qnx/qnx_opts.txt \
  -installPackage com.qnx.qnx800.quickstart.qemu -destination ~/qnxinstall/qnx800
```

## Step 4 — Boot QNX 8.0 in QEMU (5 min)

QEMU 6.x works for console use; QNX recommends QEMU 10 for graphics.
On a small host, cut RAM to 1 GB and CPUs to 1.

```sh
cd ~/qnxinstall/qnx800/images/qemu
sh unpack_qemu_image.sh          # unpacks the QSTI
source ~/qnxinstall/qnx800/qnxsdp-env.sh
cd qemu && mkqnximage --run      # boots the VM; your terminal becomes the serial console
```

Exit QEMU with `Ctrl-A x`. From a second terminal, `mkqnximage --getip`
shows the guest IP (user-mode networking).

## Step 5 — Your first deadline-guaranteed task (10 min)

The pattern that makes QNX *QNX*: a periodic task driven by a
`CLOCK_MONOTONIC` timer delivered as a **pulse** over a channel, at
`SCHED_FIFO` priority. No polling, no sleep drift:

```c
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    struct sched_param sp = { .sched_priority = 60 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    int chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, SI_MINAVAIL, _PULSE_CODE_MINAVAIL + 1, 0);
    timer_t tid; timer_create(CLOCK_MONOTONIC, &ev, &tid);
    struct itimerspec it = { {0, 10000000}, {0, 10000000} }; /* 10 ms */
    timer_settime(tid, 0, &it, NULL);

    struct timespec exp_; clock_gettime(CLOCK_MONOTONIC, &exp_);
    struct _pulse p; unsigned long worst = 0;
    for (;;) {
        MsgReceivePulse(chid, &p, sizeof p, NULL);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long late = (now.tv_sec - exp_.tv_sec) * 1000000L
                  + (now.tv_nsec - exp_.tv_nsec) / 1000;
        if (late > 0 && (unsigned long)late > worst) {
            worst = late;
            printf("new worst wake latency: %lu us\n", worst);
        }
        exp_.tv_nsec += 10000000;
        if (exp_.tv_nsec >= 1000000000L) { exp_.tv_sec++; exp_.tv_nsec -= 1000000000L; }
        /* your periodic work here */
    }
}
```

Build and run on the target:

```sh
source ~/qnxinstall/qnx800/qnxsdp-env.sh
qcc -Vgcc_ntox86_64 -O2 -o rt_hello rt_hello.c
# copy into the VM (scp via the QSTI's sftp/qconn, or rebuild the image),
# then on the QNX console:  ./rt_hello
```

## Step 6 — Prove determinism under load (5 min)

Run a best-effort busy loop at priority 1 (`nice` loop, `SCHED_OTHER`),
pinning the CPU at 100%. On a general-purpose OS your 10 ms task now
misses deadlines by milliseconds. On QNX, the worst-case latency printed
by `rt_hello` stays in the tens of microseconds — the microkernel
preempts everything below your priority, every time.

That property — not speed, *predictability* — is what this hackathon
project (VitalsGuard-RT) builds a full alarm pipeline on. See
`../src/analyzer.c` for the production version of this pattern.

## Troubleshooting

- **`Username already exists` on signup:** the email already has a myQNX
  account; use "Forgot Password" — the one-time password arrives by email
  and forces a password change on first login.
- **QSC says no products:** the license must be *deployed* to your
  account first (Step 1, second email). Re-run `-syncLicenseKeys`.
- **QEMU too slow:** drop to 1 CPU / 1 GB RAM, avoid the graphical
  launcher, use the serial console.
- **`ThreadCtl: Permission denied`:** resource managers need I/O
  privilege; run as root on the QSTI console (default login is root,
  no password).
