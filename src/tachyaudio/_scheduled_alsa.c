#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
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

typedef struct {
    PyObject_HEAD
    void *library, *pcm, *snapshot;
    AlsaApi api;
    pthread_t thread;
    pthread_mutex_t lock;
    int lock_ready, running, stopping, closed, fault, state, sample_format, rt_priority_active;
    unsigned int rate, channels;
    unsigned long capacity, period;
    float *samples, *output;
    void *integer_output;
    size_t frames, position;
    uint64_t requested, onset, callback, complete, end, callbacks, beginning;
} AlsaScheduled;

static uint64_t alsa_now(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
    return (uint64_t)stamp.tv_sec * 1000000000ULL + stamp.tv_nsec;
}

static void alsa_fail(AlsaScheduled *self, int state)
{
    self->fault = 1;
    self->state = state;
    self->api.drop(self->pcm);
}

static void *alsa_worker(void *context)
{
    AlsaScheduled *self = context;
    uint64_t previous_timestamp = 0;
    for (;;) {
        pthread_mutex_lock(&self->lock);
        if (self->stopping || self->fault) {
            pthread_mutex_unlock(&self->lock);
            break;
        }
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
        if (delay < 0 || (unsigned long)delay > self->capacity ||
            available > self->capacity || timestamp == 0 || timestamp > now ||
            timestamp < previous_timestamp ||
            now - timestamp > 100000000ULL) {
            alsa_fail(self, TACHY_SCHEDULE_CLOCK_ERROR);
            pthread_mutex_unlock(&self->lock);
            break;
        }
        previous_timestamp = timestamp;
        if (available >= self->period) {
            self->callbacks++;
            uint64_t beginning = timestamp + (uint64_t)llround(delay * 1e9 / self->rate);
            memset(self->output, 0, self->period * self->channels * sizeof(float));
            size_t count = 0;
            unsigned long offset = 0;
            int pending = self->state == TACHY_SCHEDULE_PENDING;
            if (pending || self->state == TACHY_SCHEDULE_PLAYING) {
                long long wanted = pending ? tachy_schedule_offset(self->requested, beginning, self->rate) : 0;
                if (wanted < 0) {
                    self->state = TACHY_SCHEDULE_MISSED;
                } else if ((unsigned long long)wanted < self->period) {
                    offset = (unsigned long)wanted;
                    count = self->frames - self->position;
                    if (count > self->period - offset) count = self->period - offset;
                    uint64_t first = beginning + (uint64_t)llround(offset * 1e9 / self->rate);
                    if (now >= first) {
                        alsa_fail(self, TACHY_SCHEDULE_CALLBACK_LATE);
                        pthread_mutex_unlock(&self->lock);
                        break;
                    }
                    if (pending) {
                        self->beginning = beginning;
                        self->onset = first;
                        self->callback = now;
                    }
                    memcpy(self->output + offset * self->channels,
                           self->samples + self->position * self->channels,
                           count * self->channels * sizeof(float));
                }
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
            if (written != (long)self->period) {
                alsa_fail(self, TACHY_SCHEDULE_DISCONTINUITY);
            } else if (count) {
                uint64_t first = beginning + (uint64_t)llround(offset * 1e9 / self->rate);
                if (completed >= first) {
                    alsa_fail(self, TACHY_SCHEDULE_CALLBACK_LATE);
                } else {
                    if (pending) self->complete = completed;
                    self->position += count;
                    self->state = TACHY_SCHEDULE_PLAYING;
                    if (self->position == self->frames) {
                        self->end = beginning + (uint64_t)llround((offset + count) * 1e9 / self->rate);
                        self->state = TACHY_SCHEDULE_RENDERED;
                    }
                }
            }
        }
        pthread_mutex_unlock(&self->lock);
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void alsa_release(AlsaScheduled *self)
{
    if (self->pcm) self->api.close(self->pcm);
    if (self->snapshot) self->api.status_free(self->snapshot);
    if (self->library) dlclose(self->library);
    PyMem_RawFree(self->samples);
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
    if (self->state == TACHY_SCHEDULE_PENDING || self->state == TACHY_SCHEDULE_PLAYING)
        self->state = TACHY_SCHEDULE_CANCELLED;
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

static PyObject *alsa_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    int rate = 48000, channels = 2;
    const char *device = NULL;
    static char *keywords[] = {"sample_rate", "channels", "device_id", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iiz", keywords, &rate, &channels, &device)) return NULL;
    if (rate < 1 || channels < 1 || channels > 32 || !device || strncmp(device, "hw:", 3) != 0) {
        PyErr_SetString(PyExc_ValueError, "ALSA scheduling requires device_id='hw:CARD,DEVICE', positive rate and 1-32 channels");
        return NULL;
    }
    AlsaScheduled *self = (AlsaScheduled *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    self->rate = rate;
    self->channels = channels;
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
    self->sample_format = TACHY_ALSA_SAMPLE_FLOAT32;
    if (self->api.set_params(self->pcm, TACHY_ALSA_FORMAT_FLOAT_LE, 3, channels, rate, 0, 40000) < 0) {
        self->sample_format = TACHY_ALSA_SAMPLE_S32;
        if (self->api.set_params(self->pcm, TACHY_ALSA_FORMAT_S32_LE, 3, channels, rate, 0, 40000) < 0) {
            self->sample_format = TACHY_ALSA_SAMPLE_S16;
            if (self->api.set_params(self->pcm, TACHY_ALSA_FORMAT_S16_LE, 3, channels, rate, 0, 40000) < 0) goto failure;
        }
    }
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
    self->output = PyMem_RawCalloc(self->capacity * channels, sizeof(float));
    if (!self->output) goto failure;
    if (self->sample_format == TACHY_ALSA_SAMPLE_S32) {
        self->integer_output = PyMem_RawCalloc(self->capacity * channels, sizeof(int32_t));
        if (!self->integer_output) goto failure;
    } else if (self->sample_format == TACHY_ALSA_SAMPLE_S16) {
        self->integer_output = PyMem_RawCalloc(self->capacity * channels, sizeof(int16_t));
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
    if (pthread_create(&self->thread, NULL, alsa_worker, self) != 0) {
        self->api.drop(self->pcm);
        PyErr_SetString(PyExc_RuntimeError, "could not start ALSA scheduling thread");
        return NULL;
    }
    self->running = 1;
    /* Best-effort real-time scheduling for the render worker, matching the
     * protection PsychPortAudio/PipeWire's rtkit path give their audio
     * threads. Without CAP_SYS_NICE or an RLIMIT_RTPRIO grant this fails
     * harmlessly and the worker keeps running at the normal priority. */
    struct sched_param rt_param;
    memset(&rt_param, 0, sizeof(rt_param));
    int max_priority = sched_get_priority_max(SCHED_FIFO);
    if (max_priority >= 0) {
        rt_param.sched_priority = max_priority > 10 ? max_priority - 10 : max_priority;
        self->rt_priority_active = pthread_setschedparam(self->thread, SCHED_FIFO, &rt_param) == 0;
    } else {
        self->rt_priority_active = 0;
    }
    Py_RETURN_NONE;
}

static PyObject *alsa_schedule(AlsaScheduled *self, PyObject *args)
{
    PyObject *frames;
    unsigned long long target, lead = 0;
    if (!PyArg_ParseTuple(args, "OK|K", &frames, &target, &lead)) return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) < 0) return NULL;
    size_t stride = self->channels * sizeof(float);
    if (view.len <= 0 || (size_t)view.len % stride) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "expected complete float32 frames");
        return NULL;
    }
    float *copy = PyMem_RawMalloc(view.len);
    if (!copy) { PyBuffer_Release(&view); return PyErr_NoMemory(); }
    memcpy(copy, view.buf, view.len);
    size_t count = view.len / stride;
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
    if (self->closed || self->fault || !self->running ||
        self->state == TACHY_SCHEDULE_PENDING || self->state == TACHY_SCHEDULE_PLAYING ||
        target <= now || target - now < lead) {
        pthread_mutex_unlock(&self->lock);
        PyMem_RawFree(copy);
        PyErr_SetString(PyExc_ValueError, "stream unavailable, stimulus active, or insufficient advance notice");
        return NULL;
    }
    float *old = self->samples;
    self->samples = copy;
    self->frames = count;
    self->position = 0;
    self->requested = target;
    self->onset = self->callback = self->complete = self->end = self->beginning = 0;
    self->state = TACHY_SCHEDULE_PENDING;
    pthread_mutex_unlock(&self->lock);
    PyMem_RawFree(old);
    Py_RETURN_NONE;
}

static PyObject *alsa_status(AlsaScheduled *self, PyObject *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&self->lock);
    int state = self->state;
    uint64_t requested = self->requested, onset = self->onset, callback = self->callback;
    uint64_t complete = self->complete, end = self->end, frames = self->position;
    uint64_t callbacks = self->callbacks, beginning = self->beginning;
    int rt_priority_active = self->rt_priority_active;
    pthread_mutex_unlock(&self->lock);
    const char *device_format = self->sample_format == TACHY_ALSA_SAMPLE_S32 ? "s32_le" :
        self->sample_format == TACHY_ALSA_SAMPLE_S16 ? "s16_le" : "float32_le";
    return Py_BuildValue("{s:i,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:k,s:I,s:s,s:s,s:i}",
        "state", state, "requested_ns", (unsigned long long)requested,
        "predicted_onset_ns", (unsigned long long)onset,
        "render_callback_ns", (unsigned long long)callback,
        "render_complete_ns", (unsigned long long)complete,
        "predicted_end_ns", (unsigned long long)end,
        "rendered_frames", (unsigned long long)frames,
        "callbacks", (unsigned long long)callbacks,
        "decision_buffer_ns", (unsigned long long)beginning,
        "buffer_frames", self->capacity, "sample_rate", self->rate,
        "clock_source", "alsa-monotonic-status-delay",
        "device_format", device_format,
        "rt_priority", rt_priority_active);
}

static PyObject *alsa_cancel(AlsaScheduled *self, PyObject *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&self->lock);
    if (self->state == TACHY_SCHEDULE_PENDING || self->state == TACHY_SCHEDULE_PLAYING)
        self->state = TACHY_SCHEDULE_CANCELLED;
    pthread_mutex_unlock(&self->lock);
    Py_RETURN_NONE;
}

static PyMethodDef alsa_methods[] = {
    {"start", (PyCFunction)alsa_start, METH_NOARGS, "Prime silence and start the native worker."},
    {"schedule", (PyCFunction)alsa_schedule, METH_VARARGS, "Submit a complete future stimulus."},
    {"status", (PyCFunction)alsa_status, METH_NOARGS, "Read ALSA timing estimates."},
    {"cancel", (PyCFunction)alsa_cancel, METH_NOARGS, "Cancel remaining unsupplied samples."},
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
