#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <time.h>
#include <unistd.h>
#include "_scheduled_common.h"

/* ALSA snd_pcm_format_t values used for format negotiation, best precision first. */
#define TACHY_ALSA_FORMAT_FLOAT_LE 14
#define TACHY_ALSA_FORMAT_S32_LE 10
#define TACHY_ALSA_FORMAT_S16_LE 2

enum {
    TACHY_ALSA_SAMPLE_FLOAT32 = 0,
    TACHY_ALSA_SAMPLE_S32,
    TACHY_ALSA_SAMPLE_S16
};

#ifndef TACHY_ALSA_LIBRARY
#define TACHY_ALSA_LIBRARY "libasound.so.2"
#endif

/* Device-clock model: least-squares fit of (status call time, presented frame)
 * over a sliding window. Single ALSA status reads are noisy (USB htstamps lag
 * by milliseconds), the fit is not. */
#define TACHY_ALSA_FIT_CAPACITY 4096
#define TACHY_ALSA_FIT_WINDOW_NS 2000000000ULL
#define TACHY_ALSA_FIT_SLOPE_SPAN_NS 500000000ULL
#define TACHY_ALSA_FIT_MINIMUM 8
#define TACHY_ALSA_FIT_RESET_NS 5000000LL
#define TACHY_ALSA_FIT_BRACKET_NS 200000ULL
/* USB delay reports are ~1 ms early for the first few ms after start. */
#define TACHY_ALSA_FIT_SETTLE_NS 20000000ULL

/* Automatic buffer sizes, from acoustic-loopback runs under full CPU load: with
 * RT priority 5 ms (and 3 ms) never failed; without it 5 and 10 ms faulted and
 * 20 ms did not. */
#define TACHY_ALSA_BUFFER_RT_US 5000
#define TACHY_ALSA_BUFFER_NORMAL_US 20000

typedef struct {
    int (*open)(void **, const char *, int, int);
    int (*close)(void *);
    int (*set_params)(void *, int, int, unsigned int, unsigned int, int, unsigned int);
    int (*get_params)(void *, unsigned long *, unsigned long *);
    int (*prepare)(void *);
    int (*start)(void *);
    int (*drop)(void *);
    long (*writei)(void *, const void *, unsigned long);
    int (*sw_params_malloc)(void **);
    void (*sw_params_free)(void *);
    int (*sw_params_current)(void *, void *);
    int (*sw_params_set_start_threshold)(void *, void *, unsigned long);
    int (*sw_params_set_tstamp_mode)(void *, void *, int);
    int (*sw_params_set_tstamp_type)(void *, void *, int);
    int (*sw_params)(void *, void *);
    int (*status_malloc)(void **);
    void (*status_free)(void *);
    int (*status)(void *, void *);
    int (*status_get_state)(const void *);
    unsigned long (*status_get_avail)(const void *);
    long (*status_get_delay)(const void *);
    void (*status_get_htstamp)(const void *, struct timespec *);
} AlsaApi;

/* Stimuli queued on one stream. Finished slots keep their timing record until
 * reused, so results can still be read after completion. */
#define TACHY_ALSA_QUEUE 32

typedef struct {
    uint64_t id;
    int state, started;
    float *samples;
    size_t frames, position;
    unsigned long offset, count;
    uint64_t requested, onset, callback, complete, end, beginning;
} AlsaStimulus;

typedef struct {
    PyObject_HEAD
    void *library, *pcm, *snapshot;
    AlsaApi api;
    pthread_t thread;
    pthread_mutex_t lock;
    int lock_ready, running, stopping, closed, fault, fault_state, sample_format, rt_priority, buffer_automatic;
    const char *rt_method;
    pid_t worker_tid;
    unsigned int rate, channels, input_channels;
    unsigned long capacity, period;
    float *output;
    void *integer_output;
    AlsaStimulus stimuli[TACHY_ALSA_QUEUE];
    uint64_t next_id, latest_id, callbacks, started;
    int64_t written;
    int64_t fit_frame[TACHY_ALSA_FIT_CAPACITY];
    uint64_t fit_time[TACHY_ALSA_FIT_CAPACITY];
    unsigned int fit_head, fit_count;
    uint64_t clock_resets;
    long last_delay, max_delay;
    double clock_ppm, clock_jitter_ns;
} AlsaScheduled;

static uint64_t alsa_now(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
    return (uint64_t)stamp.tv_sec * 1000000000ULL + stamp.tv_nsec;
}

static void alsa_fit_add(AlsaScheduled *self, int64_t frame, uint64_t time)
{
    self->fit_frame[self->fit_head] = frame;
    self->fit_time[self->fit_head] = time;
    self->fit_head = (self->fit_head + 1) % TACHY_ALSA_FIT_CAPACITY;
    if (self->fit_count < TACHY_ALSA_FIT_CAPACITY) self->fit_count++;
    while (self->fit_count > 1) {
        unsigned int oldest = (self->fit_head + TACHY_ALSA_FIT_CAPACITY - self->fit_count) % TACHY_ALSA_FIT_CAPACITY;
        if (time - self->fit_time[oldest] <= TACHY_ALSA_FIT_WINDOW_NS) break;
        self->fit_count--;
    }
}

static int alsa_fit_predict(AlsaScheduled *self, int64_t frame, uint64_t *predicted)
{
    unsigned int count = self->fit_count;
    if (count < TACHY_ALSA_FIT_MINIMUM) return 0;
    unsigned int newest = (self->fit_head + TACHY_ALSA_FIT_CAPACITY - 1) % TACHY_ALSA_FIT_CAPACITY;
    unsigned int oldest = (self->fit_head + TACHY_ALSA_FIT_CAPACITY - count) % TACHY_ALSA_FIT_CAPACITY;
    int64_t frame0 = self->fit_frame[newest];
    uint64_t time0 = self->fit_time[newest];
    double nominal = 1e9 / self->rate, sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (unsigned int index = 0; index < count; index++) {
        unsigned int slot = (oldest + index) % TACHY_ALSA_FIT_CAPACITY;
        double x = (double)(self->fit_frame[slot] - frame0);
        double y = (double)(int64_t)(self->fit_time[slot] - time0);
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y;
    }
    double mx = sx / count, my = sy / count;
    double vxx = sxx / count - mx * mx, vxy = sxy / count - mx * my, vyy = syy / count - my * my;
    double slope = nominal;
    if (time0 - self->fit_time[oldest] >= TACHY_ALSA_FIT_SLOPE_SPAN_NS && vxx > 0) {
        slope = vxy / vxx;
        if (fabs(slope / nominal - 1) > 1e-3) slope = nominal;
    }
    double intercept = my - slope * mx;
    double variance = vyy - 2 * slope * vxy + slope * slope * vxx;
    self->clock_ppm = (slope / nominal - 1) * 1e6;
    self->clock_jitter_ns = variance > 0 ? sqrt(variance) : 0;
    *predicted = time0 + (uint64_t)llround(intercept + slope * (double)(frame - frame0));
    return 1;
}

#ifdef __linux__
/* rtkit grants SCHED_RR to unprivileged desktop users over the system D-Bus
 * (this is how PipeWire's data thread gets RT). libdbus is loaded on demand
 * and deliberately never unloaded. */
static int alsa_rtkit_call(const char *interface, const char *method, int first_type, ...)
{
    void *dbus = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
    if (!dbus) return 0;
    void (*error_init)(void *) = (void (*)(void *))dlsym(dbus, "dbus_error_init");
    void (*error_free)(void *) = (void (*)(void *))dlsym(dbus, "dbus_error_free");
    void *(*bus_get_private)(int, void *) = (void *(*)(int, void *))dlsym(dbus, "dbus_bus_get_private");
    void (*set_exit)(void *, unsigned int) =
        (void (*)(void *, unsigned int))dlsym(dbus, "dbus_connection_set_exit_on_disconnect");
    void *(*new_call)(const char *, const char *, const char *, const char *) =
        (void *(*)(const char *, const char *, const char *, const char *))dlsym(dbus, "dbus_message_new_method_call");
    unsigned int (*append)(void *, int, va_list) =
        (unsigned int (*)(void *, int, va_list))dlsym(dbus, "dbus_message_append_args_valist");
    void *(*send)(void *, void *, int, void *) =
        (void *(*)(void *, void *, int, void *))dlsym(dbus, "dbus_connection_send_with_reply_and_block");
    void (*message_unref)(void *) = (void (*)(void *))dlsym(dbus, "dbus_message_unref");
    void (*connection_close)(void *) = (void (*)(void *))dlsym(dbus, "dbus_connection_close");
    void (*connection_unref)(void *) = (void (*)(void *))dlsym(dbus, "dbus_connection_unref");
    if (!error_init || !error_free || !bus_get_private || !set_exit || !new_call || !append ||
        !send || !message_unref || !connection_close || !connection_unref) return 0;
    union { void *pointers[8]; char bytes[64]; } error; /* larger than DBusError */
    error_init(&error);
    int succeeded = 0;
    void *connection = bus_get_private(1 /* DBUS_BUS_SYSTEM */, &error);
    if (connection) {
        set_exit(connection, 0);
        void *message = new_call("org.freedesktop.RealtimeKit1", "/org/freedesktop/RealtimeKit1",
                                 interface, method);
        va_list arguments;
        va_start(arguments, first_type);
        int appended = message && append(message, first_type, arguments);
        va_end(arguments);
        if (appended) {
            void *reply = send(connection, message, 500, &error);
            if (reply) {
                succeeded = 1;
                message_unref(reply);
            }
        }
        if (message) message_unref(message);
        connection_close(connection);
        connection_unref(connection);
    }
    error_free(&error);
    return succeeded;
}

static int alsa_rtkit(pid_t tid, unsigned int priority)
{
    uint64_t thread = (uint64_t)tid;
    uint32_t level = priority;
    return alsa_rtkit_call("org.freedesktop.RealtimeKit1", "MakeThreadRealtime",
                           't', &thread, 'u', &level, 0);
}
#endif

/* Side-effect-free check used to pick the automatic buffer size. */
static int alsa_realtime_available(void)
{
#ifdef __linux__
    struct rlimit limit;
    if (geteuid() == 0 || (getrlimit(RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur > 0)) return 1;
    const char *interface = "org.freedesktop.RealtimeKit1", *property = "MaxRealtimePriority";
    return alsa_rtkit_call("org.freedesktop.DBus.Properties", "Get",
                           's', &interface, 's', &property, 0);
#else
    return 0;
#endif
}

static int alsa_promote(AlsaScheduled *self, const char **method)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    int maximum = sched_get_priority_max(SCHED_FIFO);
    param.sched_priority = maximum > 10 ? maximum - 10 : maximum;
#ifdef __linux__
    struct rlimit allowed;
    if (geteuid() != 0 && getrlimit(RLIMIT_RTPRIO, &allowed) == 0 && allowed.rlim_cur != RLIM_INFINITY &&
        (rlim_t)param.sched_priority > allowed.rlim_cur)
        param.sched_priority = (int)allowed.rlim_cur;
#endif
    if (maximum > 0 && pthread_setschedparam(self->thread, SCHED_FIFO, &param) == 0) {
        *method = "sched_setscheduler";
        return param.sched_priority;
    }
#ifdef __linux__
    pid_t tid = 0;
    for (int attempt = 0; attempt < 1000 && !(tid = __atomic_load_n(&self->worker_tid, __ATOMIC_ACQUIRE)); attempt++) {
        struct timespec pause = {0, 100000};
        nanosleep(&pause, NULL);
    }
    if (!tid) return 0;
    /* rtkit refuses processes that could monopolize a CPU; the worker sleeps
     * every iteration, so a 200 ms RT CPU budget is never reached. */
    struct rlimit limit;
    if (getrlimit(RLIMIT_RTTIME, &limit) == 0 &&
        (limit.rlim_max == RLIM_INFINITY || limit.rlim_max > 200000)) {
        limit.rlim_cur = limit.rlim_max = 200000;
        setrlimit(RLIMIT_RTTIME, &limit);
    }
    static const unsigned int priorities[] = {20, 10, 5};
    for (size_t index = 0; index < sizeof(priorities) / sizeof(priorities[0]); index++) {
        if (alsa_rtkit(tid, priorities[index])) {
            *method = "rtkit";
            return (int)priorities[index];
        }
    }
#endif
    return 0;
}

static int alsa_active(const AlsaStimulus *stimulus)
{
    return stimulus->state == TACHY_SCHEDULE_PENDING || stimulus->state == TACHY_SCHEDULE_PLAYING;
}

/* A stream fault ends every queued stimulus; the stream must be reopened. */
static void alsa_fail(AlsaScheduled *self, int state)
{
    self->fault = 1;
    self->fault_state = state;
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++)
        if (alsa_active(&self->stimuli[index])) self->stimuli[index].state = state;
    self->api.drop(self->pcm);
}

static void *alsa_worker(void *context)
{
    AlsaScheduled *self = context;
#ifdef __linux__
    __atomic_store_n(&self->worker_tid, (pid_t)syscall(SYS_gettid), __ATOMIC_RELEASE);
#endif
    uint64_t previous_timestamp = 0;
    for (;;) {
        pthread_mutex_lock(&self->lock);
        if (self->stopping || self->fault) {
            pthread_mutex_unlock(&self->lock);
            break;
        }
        uint64_t before = alsa_now();
        if (self->api.status(self->pcm, self->snapshot) < 0 ||
            self->api.status_get_state(self->snapshot) != 3) {
            alsa_fail(self, TACHY_SCHEDULE_DISCONTINUITY);
            pthread_mutex_unlock(&self->lock);
            break;
        }
        unsigned long available = self->api.status_get_avail(self->snapshot);
        long delay = self->api.status_get_delay(self->snapshot);
        struct timespec stamp;
        self->api.status_get_htstamp(self->snapshot, &stamp);
        uint64_t now = alsa_now();
        uint64_t timestamp = (uint64_t)stamp.tv_sec * 1000000000ULL + stamp.tv_nsec;
        /* USB audio reports frames already submitted to URBs as delay, so it can
         * exceed the ring buffer; allow up to 100 ms beyond the queued frames. */
        if (delay < 0 || available > self->capacity ||
            (unsigned long)delay > self->capacity - available + self->rate / 10 ||
            timestamp == 0 || timestamp > now ||
            timestamp < previous_timestamp ||
            now - timestamp > 100000000ULL) {
            alsa_fail(self, TACHY_SCHEDULE_CLOCK_ERROR);
            pthread_mutex_unlock(&self->lock);
            break;
        }
        previous_timestamp = timestamp;
        self->last_delay = delay;
        uint64_t observed = before + (now - before) / 2;
        int64_t presented = self->written - delay;
        if (available >= self->period) {
            self->callbacks++;
            uint64_t beginning = observed + (uint64_t)llround(delay * 1e9 / self->rate);
            uint64_t fitted;
            if (alsa_fit_predict(self, self->written, &fitted)) {
                if (llabs((long long)(fitted - beginning)) > TACHY_ALSA_FIT_RESET_NS) {
                    self->clock_resets++;
                    self->fit_count = 0;
                } else {
                    beginning = fitted;
                }
            }
            memset(self->output, 0, self->period * self->channels * sizeof(float));
            int late = 0;
            for (int index = 0; index < TACHY_ALSA_QUEUE && !late; index++) {
                AlsaStimulus *stimulus = &self->stimuli[index];
                stimulus->count = 0;
                stimulus->started = 0;
                if (!alsa_active(stimulus)) continue;
                unsigned long offset = 0;
                if (stimulus->state == TACHY_SCHEDULE_PENDING) {
                    long long wanted = tachy_schedule_offset(stimulus->requested, beginning, self->rate);
                    if (wanted < 0) {
                        stimulus->state = TACHY_SCHEDULE_MISSED;
                        continue;
                    }
                    if ((unsigned long long)wanted >= self->period) continue;
                    offset = (unsigned long)wanted;
                    uint64_t first = beginning + (uint64_t)llround(offset * 1e9 / self->rate);
                    if (now >= first) {
                        late = 1;
                        break;
                    }
                    stimulus->beginning = beginning;
                    stimulus->onset = first;
                    stimulus->callback = now;
                    stimulus->started = 1;
                }
                size_t count = stimulus->frames - stimulus->position;
                if (count > self->period - offset) count = self->period - offset;
                float *destination = self->output + offset * self->channels;
                const float *source = stimulus->samples + stimulus->position * self->channels;
                for (size_t sample = 0; sample < count * self->channels; sample++)
                    destination[sample] += source[sample];
                stimulus->offset = offset;
                stimulus->count = count;
            }
            if (late) {
                alsa_fail(self, TACHY_SCHEDULE_CALLBACK_LATE);
                pthread_mutex_unlock(&self->lock);
                break;
            }
            const void *output = self->output;
            if (self->sample_format == TACHY_ALSA_SAMPLE_S32) {
                int32_t *destination = (int32_t *)self->integer_output;
                for (size_t index = 0; index < self->period * self->channels; index++) {
                    float value = self->output[index];
                    destination[index] = value >= (2147483647.0f / 2147483648.0f) ? INT32_MAX :
                        value <= -1 ? INT32_MIN : (int32_t)llrint((double)value * 2147483648.0);
                }
                output = self->integer_output;
            } else if (self->sample_format == TACHY_ALSA_SAMPLE_S16) {
                int16_t *destination = (int16_t *)self->integer_output;
                for (size_t index = 0; index < self->period * self->channels; index++) {
                    float value = self->output[index];
                    destination[index] = value >= (32767.0f / 32768.0f) ? INT16_MAX :
                        value <= -1 ? INT16_MIN : (int16_t)lrintf(value * 32768.0f);
                }
                output = self->integer_output;
            }
            long written = self->api.writei(self->pcm, output, self->period);
            uint64_t completed = alsa_now();
            if (written == (long)self->period) self->written += written;
            if (written != (long)self->period) {
                alsa_fail(self, TACHY_SCHEDULE_DISCONTINUITY);
            } else {
                for (int index = 0; index < TACHY_ALSA_QUEUE && !self->fault; index++) {
                    AlsaStimulus *stimulus = &self->stimuli[index];
                    if (!stimulus->count || !alsa_active(stimulus)) continue;
                    uint64_t first = beginning + (uint64_t)llround(stimulus->offset * 1e9 / self->rate);
                    if (completed >= first) {
                        alsa_fail(self, TACHY_SCHEDULE_CALLBACK_LATE);
                        break;
                    }
                    if (stimulus->started) stimulus->complete = completed;
                    stimulus->position += stimulus->count;
                    stimulus->state = TACHY_SCHEDULE_PLAYING;
                    if (stimulus->position == stimulus->frames) {
                        stimulus->end = beginning + (uint64_t)llround(
                            (stimulus->offset + stimulus->count) * 1e9 / self->rate);
                        stimulus->state = TACHY_SCHEDULE_RENDERED;
                    }
                }
            }
        }
        /* A preempted status call cannot be placed in time; keep tight brackets only. */
        if (observed - self->started >= TACHY_ALSA_FIT_SETTLE_NS) {
            if (delay > self->max_delay) self->max_delay = delay;
            if (now - before <= TACHY_ALSA_FIT_BRACKET_NS) alsa_fit_add(self, presented, observed);
        }
        pthread_mutex_unlock(&self->lock);
        long pause_ns = (long)(self->period * 500000000.0 / self->rate);
        struct timespec pause = {0, pause_ns < 1000000 ? pause_ns : 1000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void alsa_release(AlsaScheduled *self)
{
    if (self->pcm) self->api.close(self->pcm);
    if (self->snapshot) self->api.status_free(self->snapshot);
    if (self->library) dlclose(self->library);
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++) PyMem_RawFree(self->stimuli[index].samples);
    PyMem_RawFree(self->output);
    PyMem_RawFree(self->integer_output);
    if (self->lock_ready) pthread_mutex_destroy(&self->lock);
}

static PyObject *alsa_close(AlsaScheduled *self, PyObject *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&self->lock);
    if (self->closed) {
        pthread_mutex_unlock(&self->lock);
        Py_RETURN_NONE;
    }
    self->closed = 1;
    self->stopping = 1;
    pthread_mutex_unlock(&self->lock);
    if (self->running) {
        Py_BEGIN_ALLOW_THREADS
        pthread_join(self->thread, NULL);
        Py_END_ALLOW_THREADS
        self->running = 0;
    }
    self->api.drop(self->pcm);
    self->api.close(self->pcm);
    self->pcm = NULL;
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++)
        if (alsa_active(&self->stimuli[index])) self->stimuli[index].state = TACHY_SCHEDULE_CANCELLED;
    Py_RETURN_NONE;
}

static void alsa_dealloc(AlsaScheduled *self)
{
    if (self->running) {
        PyObject *result = alsa_close(self, NULL);
        Py_XDECREF(result);
    }
    alsa_release(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static void alsa_quiet(const char *file, int line, const char *function, int error, const char *format, ...)
{
    (void)file; (void)line; (void)function; (void)error; (void)format;
}

static int alsa_configure(AlsaScheduled *self, unsigned int channels, unsigned int buffer_us)
{
    static const int formats[][2] = {
        {TACHY_ALSA_FORMAT_FLOAT_LE, TACHY_ALSA_SAMPLE_FLOAT32},
        {TACHY_ALSA_FORMAT_S32_LE, TACHY_ALSA_SAMPLE_S32},
        {TACHY_ALSA_FORMAT_S16_LE, TACHY_ALSA_SAMPLE_S16},
    };
    for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); index++) {
        if (self->api.set_params(self->pcm, formats[index][0], 3, channels, self->rate, 0, buffer_us) >= 0) {
            self->sample_format = formats[index][1];
            return 1;
        }
    }
    return 0;
}

static PyObject *alsa_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    int rate = 48000, channels = 2;
    unsigned int buffer_us = 0;
    const char *device = NULL;
    static char *keywords[] = {"sample_rate", "channels", "device_id", "buffer_us", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iizI", keywords, &rate, &channels, &device, &buffer_us)) return NULL;
    if (rate < 1 || channels < 1 || channels > 32 || !device || strncmp(device, "hw:", 3) != 0 ||
        (buffer_us != 0 && (buffer_us < 1000 || buffer_us > 1000000))) {
        PyErr_SetString(PyExc_ValueError, "ALSA scheduling requires device_id='hw:CARD,DEVICE', positive rate, 1-32 channels and a 1-1000 ms buffer");
        return NULL;
    }
    int automatic = buffer_us == 0;
    if (automatic) {
        int realtime;
        Py_BEGIN_ALLOW_THREADS
        realtime = alsa_realtime_available();
        Py_END_ALLOW_THREADS
        buffer_us = realtime ? TACHY_ALSA_BUFFER_RT_US : TACHY_ALSA_BUFFER_NORMAL_US;
    }
    AlsaScheduled *self = (AlsaScheduled *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    self->rate = rate;
    self->channels = self->input_channels = channels;
    self->buffer_automatic = automatic;
    self->rt_method = "none";
    if (pthread_mutex_init(&self->lock, NULL) != 0) goto failure;
    self->lock_ready = 1;
    self->library = dlopen(TACHY_ALSA_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (!self->library) goto failure;
#define LOAD_ALSA(name) do { *(void **)(&self->api.name) = dlsym(self->library, "snd_pcm_" #name); if (!self->api.name) goto failure; } while (0)
    LOAD_ALSA(open); LOAD_ALSA(close); LOAD_ALSA(set_params); LOAD_ALSA(get_params);
    LOAD_ALSA(prepare); LOAD_ALSA(start); LOAD_ALSA(drop); LOAD_ALSA(writei);
    LOAD_ALSA(sw_params_malloc); LOAD_ALSA(sw_params_free); LOAD_ALSA(sw_params_current);
    LOAD_ALSA(sw_params_set_start_threshold); LOAD_ALSA(sw_params_set_tstamp_mode);
    LOAD_ALSA(sw_params_set_tstamp_type); LOAD_ALSA(sw_params);
    LOAD_ALSA(status_malloc); LOAD_ALSA(status_free); LOAD_ALSA(status);
    LOAD_ALSA(status_get_state); LOAD_ALSA(status_get_avail); LOAD_ALSA(status_get_delay);
    LOAD_ALSA(status_get_htstamp);
#undef LOAD_ALSA
    if (self->api.open(&self->pcm, device, 0, 1) < 0) goto failure;
    /* Rejected format/channel attempts are expected; keep libasound from
     * printing them, then restore its default error handler. */
    int (*set_error_handler)(void *) = (int (*)(void *))dlsym(self->library, "snd_lib_error_set_handler");
    if (set_error_handler) set_error_handler((void *)alsa_quiet);
    /* Mono stimuli are duplicated onto stereo-only devices, as on macOS. */
    int configured = alsa_configure(self, (unsigned int)channels, buffer_us);
    if (!configured && channels == 1 && alsa_configure(self, 2, buffer_us)) {
        self->channels = 2;
        configured = 1;
    }
    if (set_error_handler) set_error_handler(NULL);
    if (!configured) goto failure;
    if (self->api.get_params(self->pcm, &self->capacity, &self->period) < 0 ||
        self->period == 0 || self->capacity < self->period * 2 || self->capacity > (unsigned long)rate) goto failure;
    void *params = NULL;
    if (self->api.sw_params_malloc(&params) < 0) goto failure;
    int result = self->api.sw_params_current(self->pcm, params);
    if (result >= 0) result = self->api.sw_params_set_start_threshold(self->pcm, params, self->capacity);
    if (result >= 0) result = self->api.sw_params_set_tstamp_mode(self->pcm, params, 1);
    if (result >= 0) result = self->api.sw_params_set_tstamp_type(self->pcm, params, 1);
    if (result >= 0) result = self->api.sw_params(self->pcm, params);
    self->api.sw_params_free(params);
    if (result < 0 || self->api.status_malloc(&self->snapshot) < 0) goto failure;
    self->output = PyMem_RawCalloc(self->capacity * self->channels, sizeof(float));
    if (!self->output) goto failure;
    if (self->sample_format == TACHY_ALSA_SAMPLE_S32) {
        self->integer_output = PyMem_RawCalloc(self->capacity * self->channels, sizeof(int32_t));
        if (!self->integer_output) goto failure;
    } else if (self->sample_format == TACHY_ALSA_SAMPLE_S16) {
        self->integer_output = PyMem_RawCalloc(self->capacity * self->channels, sizeof(int16_t));
        if (!self->integer_output) goto failure;
    }
    return (PyObject *)self;
failure:
    PyErr_SetString(PyExc_RuntimeError, "ALSA scheduler setup failed: requires libasound.so.2, available hw: device, float32/signed-32/signed-16 interleaved format at exact requested rate/channels, and monotonic timestamps");
    Py_DECREF(self);
    return NULL;
}

static PyObject *alsa_start(AlsaScheduled *self, PyObject *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&self->lock);
    int unavailable = self->closed || self->fault;
    pthread_mutex_unlock(&self->lock);
    if (unavailable) {
        PyErr_SetString(PyExc_RuntimeError, "ALSA scheduled stream is closed or failed; reopen it");
        return NULL;
    }
    if (self->running) Py_RETURN_NONE;
    memset(self->output, 0, self->capacity * self->channels * sizeof(float));
    if (self->sample_format == TACHY_ALSA_SAMPLE_S32)
        memset(self->integer_output, 0, self->capacity * self->channels * sizeof(int32_t));
    else if (self->sample_format == TACHY_ALSA_SAMPLE_S16)
        memset(self->integer_output, 0, self->capacity * self->channels * sizeof(int16_t));
    void *primer = self->sample_format == TACHY_ALSA_SAMPLE_FLOAT32 ?
        (void *)self->output : self->integer_output;
    if (self->api.prepare(self->pcm) < 0 ||
        self->api.writei(self->pcm, primer, self->capacity) != (long)self->capacity) {
        self->fault = 1;
        PyErr_SetString(PyExc_RuntimeError, "could not prime ALSA with silence");
        return NULL;
    }
    self->written = (int64_t)self->capacity;
    self->fit_count = 0;
    self->max_delay = 0;
    self->started = alsa_now();
    self->worker_tid = 0;
    if (pthread_create(&self->thread, NULL, alsa_worker, self) != 0) {
        self->api.drop(self->pcm);
        PyErr_SetString(PyExc_RuntimeError, "could not start ALSA scheduling thread");
        return NULL;
    }
    self->running = 1;
    const char *method = "none";
    int priority;
    Py_BEGIN_ALLOW_THREADS
    priority = alsa_promote(self, &method);
    /* Return with a usable clock model so an immediate schedule is placed as
     * precisely as a later one. */
    for (int attempt = 0; attempt < 250; attempt++) {
        pthread_mutex_lock(&self->lock);
        int ready = self->fault || self->fit_count >= TACHY_ALSA_FIT_MINIMUM;
        pthread_mutex_unlock(&self->lock);
        if (ready) break;
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    Py_END_ALLOW_THREADS
    pthread_mutex_lock(&self->lock);
    self->rt_priority = priority;
    self->rt_method = method;
    pthread_mutex_unlock(&self->lock);
    Py_RETURN_NONE;
}

static PyObject *alsa_schedule(AlsaScheduled *self, PyObject *args)
{
    PyObject *frames;
    unsigned long long target, lead = 0;
    if (!PyArg_ParseTuple(args, "OK|K", &frames, &target, &lead)) return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) < 0) return NULL;
    size_t stride = self->input_channels * sizeof(float);
    if (view.len <= 0 || (size_t)view.len % stride) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "expected complete float32 frames");
        return NULL;
    }
    size_t count = view.len / stride;
    float *copy = PyMem_RawMalloc(count * self->channels * sizeof(float));
    if (!copy) { PyBuffer_Release(&view); return PyErr_NoMemory(); }
    if (self->input_channels == self->channels) {
        memcpy(copy, view.buf, view.len);
    } else {
        const float *mono = view.buf;
        for (size_t frame = 0; frame < count; frame++)
            for (unsigned int channel = 0; channel < self->channels; channel++)
                copy[frame * self->channels + channel] = mono[frame];
    }
    PyBuffer_Release(&view);
    for (size_t index = 0; index < count * self->channels; index++) {
        if (!isfinite(copy[index])) {
            PyMem_RawFree(copy);
            PyErr_SetString(PyExc_ValueError, "stimulus samples must be finite");
            return NULL;
        }
    }
    pthread_mutex_lock(&self->lock);
    uint64_t now = alsa_now();
    const char *problem = NULL;
    if (self->closed || self->fault || !self->running)
        problem = "stream is not running (closed, failed or not started)";
    else if (target <= now || target - now < lead)
        problem = "insufficient advance notice";
    /* Back-to-back stimuli (within half a sample) are accepted; overlaps are not. */
    double sample_ns = 1e9 / self->rate, duration_ns = count * sample_ns, tolerance = sample_ns / 2;
    AlsaStimulus *slot = NULL;
    for (int index = 0; index < TACHY_ALSA_QUEUE && !problem; index++) {
        AlsaStimulus *stimulus = &self->stimuli[index];
        if (alsa_active(stimulus)) {
            double start = (double)stimulus->requested, end = start + stimulus->frames * sample_ns;
            if ((double)target + tolerance < end && start + tolerance < (double)target + duration_ns)
                problem = "overlaps a stimulus already scheduled on this stream";
        } else if (!slot || stimulus->id < slot->id) {
            slot = stimulus;
        }
    }
    if (!problem && !slot) problem = "too many stimuli queued (32)";
    if (problem) {
        pthread_mutex_unlock(&self->lock);
        PyMem_RawFree(copy);
        PyErr_SetString(PyExc_ValueError, problem);
        return NULL;
    }
    float *old = slot->samples;
    memset(slot, 0, sizeof(*slot));
    slot->id = ++self->next_id;
    slot->samples = copy;
    slot->frames = count;
    slot->requested = target;
    slot->state = TACHY_SCHEDULE_PENDING;
    self->latest_id = slot->id;
    uint64_t id = slot->id;
    pthread_mutex_unlock(&self->lock);
    PyMem_RawFree(old);
    return PyLong_FromUnsignedLongLong(id);
}

static AlsaStimulus *alsa_find(AlsaScheduled *self, uint64_t id)
{
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++)
        if (id && self->stimuli[index].id == id) return &self->stimuli[index];
    return NULL;
}

static PyObject *alsa_stimulus_dict(const AlsaStimulus *stimulus, int state)
{
    return Py_BuildValue("{s:K,s:i,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
        "stimulus", (unsigned long long)stimulus->id, "state", state,
        "requested_ns", (unsigned long long)stimulus->requested,
        "predicted_onset_ns", (unsigned long long)stimulus->onset,
        "render_callback_ns", (unsigned long long)stimulus->callback,
        "render_complete_ns", (unsigned long long)stimulus->complete,
        "predicted_end_ns", (unsigned long long)stimulus->end,
        "rendered_frames", (unsigned long long)stimulus->position,
        "decision_buffer_ns", (unsigned long long)stimulus->beginning);
}

static PyObject *alsa_result(AlsaScheduled *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id)) return NULL;
    pthread_mutex_lock(&self->lock);
    AlsaStimulus *found = alsa_find(self, id ? id : self->latest_id);
    AlsaStimulus copy;
    if (found) copy = *found;
    pthread_mutex_unlock(&self->lock);
    if (!found) {
        PyErr_SetString(PyExc_LookupError, "unknown stimulus id, or its record was reused after 32 newer stimuli");
        return NULL;
    }
    return alsa_stimulus_dict(&copy, copy.state);
}

static PyObject *alsa_status(AlsaScheduled *self, PyObject *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&self->lock);
    AlsaStimulus latest;
    memset(&latest, 0, sizeof(latest));
    AlsaStimulus *found = alsa_find(self, self->latest_id);
    if (found) latest = *found;
    int state = self->fault ? self->fault_state : latest.state;
    unsigned int queued = 0;
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++) queued += alsa_active(&self->stimuli[index]);
    uint64_t requested = latest.requested, onset = latest.onset, callback = latest.callback;
    uint64_t complete = latest.complete, end = latest.end, frames = latest.position;
    uint64_t callbacks = self->callbacks, beginning = latest.beginning;
    int rt_priority = self->rt_priority;
    const char *rt_method = self->rt_method;
    unsigned int observations = self->fit_count;
    uint64_t clock_resets = self->clock_resets;
    double clock_ppm = self->clock_ppm, clock_jitter_ns = self->clock_jitter_ns;
    double delay_us = self->last_delay * 1e6 / self->rate;
    double max_delay_us = self->max_delay * 1e6 / self->rate;
    pthread_mutex_unlock(&self->lock);
    const char *device_format = self->sample_format == TACHY_ALSA_SAMPLE_S32 ? "s32_le" :
        self->sample_format == TACHY_ALSA_SAMPLE_S16 ? "s16_le" : "float32_le";
    return Py_BuildValue("{s:i,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:k,s:k,s:I,s:s,s:s,s:i,s:s,s:I,s:K,s:d,s:d,s:d,s:d,s:I,s:O,s:K,s:I}",
        "state", state, "requested_ns", (unsigned long long)requested,
        "predicted_onset_ns", (unsigned long long)onset,
        "render_callback_ns", (unsigned long long)callback,
        "render_complete_ns", (unsigned long long)complete,
        "predicted_end_ns", (unsigned long long)end,
        "rendered_frames", (unsigned long long)frames,
        "callbacks", (unsigned long long)callbacks,
        "decision_buffer_ns", (unsigned long long)beginning,
        "buffer_frames", self->capacity, "period_frames", self->period, "sample_rate", self->rate,
        "clock_source", "alsa-status-delay-fit",
        "device_format", device_format,
        "rt_priority", rt_priority,
        "rt_method", rt_method,
        "clock_observations", observations,
        "clock_resets", (unsigned long long)clock_resets,
        "clock_ppm", clock_ppm,
        "clock_jitter_us", clock_jitter_ns / 1000.0,
        "output_delay_us", delay_us,
        "output_delay_max_us", max_delay_us,
        "device_channels", self->channels,
        "buffer_automatic", self->buffer_automatic ? Py_True : Py_False,
        "stimulus", (unsigned long long)latest.id, "queued", queued);
}

static PyObject *alsa_cancel(AlsaScheduled *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id)) return NULL;
    pthread_mutex_lock(&self->lock);
    for (int index = 0; index < TACHY_ALSA_QUEUE; index++) {
        AlsaStimulus *stimulus = &self->stimuli[index];
        if ((!id || stimulus->id == id) && alsa_active(stimulus)) stimulus->state = TACHY_SCHEDULE_CANCELLED;
    }
    pthread_mutex_unlock(&self->lock);
    Py_RETURN_NONE;
}

static PyMethodDef alsa_methods[] = {
    {"start", (PyCFunction)alsa_start, METH_NOARGS, "Prime silence and start the native worker."},
    {"schedule", (PyCFunction)alsa_schedule, METH_VARARGS, "Queue a complete future stimulus; returns its id."},
    {"result", (PyCFunction)alsa_result, METH_VARARGS, "Timing record of one stimulus (default: latest)."},
    {"status", (PyCFunction)alsa_status, METH_NOARGS, "Stream diagnostics and the latest stimulus."},
    {"cancel", (PyCFunction)alsa_cancel, METH_VARARGS, "Cancel one stimulus, or all queued ones."},
    {"close", (PyCFunction)alsa_close, METH_NOARGS, "Stop and close ALSA."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject AlsaScheduledType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._scheduled_alsa.ScheduledOutputStream",
    .tp_basicsize = sizeof(AlsaScheduled),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = alsa_new,
    .tp_dealloc = (destructor)alsa_dealloc,
    .tp_methods = alsa_methods,
};

static struct PyModuleDef alsa_module = {
    PyModuleDef_HEAD_INIT, "_scheduled_alsa", "Experimental direct ALSA scheduling.", -1, NULL
};

PyMODINIT_FUNC PyInit__scheduled_alsa(void)
{
    if (PyType_Ready(&AlsaScheduledType) < 0) return NULL;
    PyObject *module = PyModule_Create(&alsa_module);
    if (!module) return NULL;
    Py_INCREF(&AlsaScheduledType);
    if (PyModule_AddObject(module, "ScheduledOutputStream", (PyObject *)&AlsaScheduledType) < 0) {
        Py_DECREF(&AlsaScheduledType);
        Py_DECREF(module);
        return NULL;
    }
    return module;
}
