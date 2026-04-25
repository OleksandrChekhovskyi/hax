/* SPDX-License-Identifier: MIT */
#include "spinner.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

/* Braille spinner — ten frames at 80ms each give ~12fps, enough to feel alive
 * without being distracting. The thread owns one terminal line: visible=1
 * means the cursor's column is mid-frame and the next write must erase first. */
static const char *const FRAMES[] = {
    "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
    "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
};
#define N_FRAMES          (sizeof(FRAMES) / sizeof(FRAMES[0]))
#define FRAME_INTERVAL_MS 80

struct spinner {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char *label;
    int visible; /* drawn on terminal right now */
    int stop;    /* thread should exit */
    size_t frame;
    int started; /* thread was successfully created */
    int enabled; /* stdout is a tty */
};

static void erase_line_locked(void)
{
    fputs("\r\x1b[K", stdout);
    fflush(stdout);
}

static void draw_frame_locked(struct spinner *s)
{
    fputs("\r\x1b[2m", stdout);
    fputs(FRAMES[s->frame], stdout);
    fputc(' ', stdout);
    fputs(s->label, stdout);
    fputs("\x1b[0m", stdout);
    fflush(stdout);
}

static void *spinner_thread(void *arg)
{
    struct spinner *s = arg;

    /* Block all signals so SIGINT/SIGPIPE/SIGWINCH are delivered to the
     * main thread, which owns libedit, libcurl, and stdout's line state. */
    sigset_t mask;
    sigfillset(&mask);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    pthread_mutex_lock(&s->mu);
    while (!s->stop) {
        if (s->visible) {
            draw_frame_locked(s);
            s->frame = (s->frame + 1) % N_FRAMES;

            /* CLOCK_REALTIME: pthread_cond_timedwait portably uses it
             * (macOS lacks pthread_condattr_setclock). Wall-clock jumps
             * during a turn are negligible for 80ms-grain animation. */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += FRAME_INTERVAL_MS * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            pthread_cond_timedwait(&s->cv, &s->mu, &ts);
        } else {
            pthread_cond_wait(&s->cv, &s->mu);
        }
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

struct spinner *spinner_new(const char *label)
{
    struct spinner *s = xcalloc(1, sizeof(*s));
    s->label = xstrdup(label && *label ? label : "Working...");
    s->enabled = isatty(fileno(stdout));
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);

    if (s->enabled) {
        if (pthread_create(&s->thread, NULL, spinner_thread, s) == 0)
            s->started = 1;
    }
    return s;
}

void spinner_show(struct spinner *s)
{
    if (!s || !s->started)
        return;
    pthread_mutex_lock(&s->mu);
    int was = s->visible;
    s->visible = 1;
    if (!was) {
        /* Draw frame 0 synchronously so the spinner is on screen the
         * moment this returns, instead of waiting for the thread to be
         * scheduled. The thread will redraw frame 0 on its next wake
         * (same glyph, no visible change), then animate at 80ms cadence. */
        s->frame = 0;
        draw_frame_locked(s);
    }
    pthread_mutex_unlock(&s->mu);
    if (!was)
        pthread_cond_signal(&s->cv);
}

void spinner_hide(struct spinner *s)
{
    if (!s || !s->started)
        return;
    pthread_mutex_lock(&s->mu);
    if (s->visible) {
        erase_line_locked();
        s->visible = 0;
    }
    pthread_mutex_unlock(&s->mu);
}

void spinner_free(struct spinner *s)
{
    if (!s)
        return;
    if (s->started) {
        pthread_mutex_lock(&s->mu);
        if (s->visible) {
            erase_line_locked();
            s->visible = 0;
        }
        s->stop = 1;
        pthread_mutex_unlock(&s->mu);
        pthread_cond_signal(&s->cv);
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s->label);
    free(s);
}
