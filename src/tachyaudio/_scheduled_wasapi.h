#include "_scheduled_common.h"

#define TACHY_WASAPI_QUEUE 32

static const IID tachy_iid_audio_clock = {
    0xcd63314f, 0x3fba, 0x4a1b, {0x81, 0x2c, 0xef, 0x96, 0x35, 0x87, 0x28, 0xe7}
};

typedef struct {
    float *samples;
    size_t frames, rendered;
    uint64_t id, target, onset, callback, complete, end;
    int state;
} TachyWasapiStimulus;

typedef struct {
    PyObject_HEAD
    SRWLOCK lock;
    HANDLE thread, stop, ready;
    WCHAR endpoint[256];
    unsigned int rate, channels, device_channels, period, buffer;
    uint64_t qpc_frequency, callbacks, observations, delay, max_delay, latest;
    uint64_t latency_ns;
    HRESULT error;
    int fault, closed, mmcss;
    TachyWasapiStimulus slots[TACHY_WASAPI_QUEUE];
} TachyScheduledOutput;

static uint64_t tachy_wasapi_now(TachyScheduledOutput *self)
{
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    uint64_t count = ticks.QuadPart, frequency = self->qpc_frequency;
    return count / frequency * 1000000000ULL +
        count % frequency * 1000000000ULL / frequency;
}

static uint64_t tachy_wasapi_frame_ns(TachyScheduledOutput *self, size_t frames)
{
    return (uint64_t)llround((double)frames * 1e9 / self->rate);
}

static int tachy_wasapi_active(const TachyWasapiStimulus *slot)
{
    return slot->state == TACHY_SCHEDULE_PENDING || slot->state == TACHY_SCHEDULE_PLAYING;
}

static void tachy_wasapi_mix(
    TachyScheduledOutput *self, double *mix, float *output, UINT32 frames,
    uint64_t begin, uint64_t now)
{
    unsigned int channels = self->device_channels;
    size_t samples = (size_t)frames * channels;
    memset(mix, 0, samples * sizeof(*mix));
    for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
        TachyWasapiStimulus *slot = &self->slots[i];
        if (!tachy_wasapi_active(slot)) continue;
        size_t offset = 0;
        if (slot->state == TACHY_SCHEDULE_PENDING) {
            long long placement = tachy_schedule_offset(slot->target, begin, self->rate);
            if (placement < 0) {
                slot->state = TACHY_SCHEDULE_MISSED;
                continue;
            }
            if ((uint64_t)placement >= frames) continue;
            offset = (size_t)placement;
            slot->onset = begin + tachy_wasapi_frame_ns(self, offset);
            slot->callback = now;
            slot->state = TACHY_SCHEDULE_PLAYING;
        }
        size_t count = slot->frames - slot->rendered;
        if (count > frames - offset) count = frames - offset;
        for (size_t frame = 0; frame < count; ++frame) {
            const float *source = slot->samples + (slot->rendered + frame) * self->channels;
            double *destination = mix + (offset + frame) * channels;
            for (unsigned int channel = 0; channel < channels; ++channel) {
                destination[channel] += source[self->channels == 1 ? 0 : channel];
            }
        }
        slot->rendered += count;
        slot->end = begin + tachy_wasapi_frame_ns(self, offset + count);
        if (slot->rendered == slot->frames) {
            slot->state = TACHY_SCHEDULE_RENDERED;
        }
    }
    for (size_t i = 0; i < samples; ++i) {
        output[i] = (float)fmax(-1.0, fmin(1.0, mix[i]));
    }
}

static DWORD WINAPI tachy_wasapi_worker(void *context)
{
    TachyScheduledOutput *self = context;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    IAudioClock *clock = NULL;
    WAVEFORMATEX *format = NULL;
    double *mix = NULL;
    HANDLE events[] = {self->stop, NULL};
    HANDLE mmcss = NULL;
    DWORD task = 0;
    UINT32 buffer = 0;
    UINT64 frequency = 0, submitted = 0, previous_position = 0;
    REFERENCE_TIME period = 0, latency = 0;
    int started = 0, ready = 0, fault = TACHY_SCHEDULE_FORMAT_ERROR;
    HRESULT initialized = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = initialized;
    BYTE *data;

    if (FAILED(hr)) goto done;
    hr = CoCreateInstance(&MA_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
        &MA_IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr)) goto done;
    if (self->endpoint[0]) {
        hr = IMMDeviceEnumerator_GetDevice(enumerator, self->endpoint, &device);
    } else {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    }
    if (FAILED(hr)) goto done;
    hr = IMMDevice_Activate(device, &MA_IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetMixFormat(client, &format);
    if (FAILED(hr)) goto done;
    const WAVEFORMATEXTENSIBLE *extended = (const WAVEFORMATEXTENSIBLE *)format;
    int floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22 &&
         IsEqualGUID(&extended->SubFormat, &MA_GUID_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    if (!floating || format->wBitsPerSample != 32 ||
        format->nBlockAlign != format->nChannels * sizeof(float) ||
        format->nSamplesPerSec != self->rate ||
        (self->channels != 1 && self->channels != format->nChannels)) {
        hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        goto done;
    }
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, format, NULL);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetDevicePeriod(client, &period, NULL);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetStreamLatency(client, &latency);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetBufferSize(client, &buffer);
    if (FAILED(hr)) goto done;
    mix = PyMem_RawCalloc((size_t)buffer * format->nChannels, sizeof(*mix));
    if (!mix) { hr = E_OUTOFMEMORY; goto done; }
    events[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!events[1]) { hr = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    hr = IAudioClient_SetEventHandle(client, events[1]);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetService(client, &MA_IID_IAudioRenderClient, (void **)&render);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetService(client, &tachy_iid_audio_clock, (void **)&clock);
    if (FAILED(hr)) goto done;
    hr = IAudioClock_GetFrequency(clock, &frequency);
    if (FAILED(hr) || !frequency) { if (SUCCEEDED(hr)) hr = E_FAIL; goto done; }
    hr = IAudioRenderClient_GetBuffer(render, buffer, &data);
    if (FAILED(hr)) goto done;
    hr = IAudioRenderClient_ReleaseBuffer(render, buffer, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) goto done;
    submitted = buffer;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    hr = IAudioClient_Start(client);
    if (FAILED(hr)) goto done;
    started = 1;
    fault = TACHY_SCHEDULE_CLOCK_ERROR;
    AcquireSRWLockExclusive(&self->lock);
    self->buffer = buffer;
    self->period = (unsigned int)llround((double)period * self->rate / 1e7);
    self->device_channels = format->nChannels;
    self->latency_ns = (uint64_t)latency * 100;
    self->mmcss = mmcss != NULL;
    ReleaseSRWLockExclusive(&self->lock);

    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait == WAIT_OBJECT_0) { hr = S_OK; break; }
        if (wait != WAIT_OBJECT_0 + 1) { hr = E_FAIL; break; }
        UINT32 padding;
        hr = IAudioClient_GetCurrentPadding(client, &padding);
        if (FAILED(hr)) break;
        if (padding >= buffer) continue;
        UINT32 frames = buffer - padding;
        UINT64 position, qpc;
        hr = IAudioClock_GetPosition(clock, &position, &qpc);
        if (FAILED(hr)) break;
        if (position < previous_position) {
            fault = TACHY_SCHEDULE_DISCONTINUITY;
            hr = E_FAIL;
            break;
        }
        previous_position = position;
        uint64_t now = tachy_wasapi_now(self);
        // GetPosition correlates playback position with QPC in 100 ns units.
        // 'submitted' is the stream frame index of the first writable frame.
        double ahead = (double)submitted / self->rate - (double)position / frequency;
        int valid = hr == S_OK && position > 0 && qpc > 0;
        uint64_t begin = valid && ahead >= 0 ? qpc * 100 +
            (uint64_t)llround(ahead * 1e9) : 0;
        hr = IAudioRenderClient_GetBuffer(render, frames, &data);
        if (FAILED(hr)) break;
        AcquireSRWLockExclusive(&self->lock);
        ++self->callbacks;
        DWORD flags = 0;
        if (begin > now) {
            ++self->observations;
            self->delay = begin - now;
            if (self->delay > self->max_delay) self->max_delay = self->delay;
            tachy_wasapi_mix(self, mix, (float *)data, frames, begin, now);
            if (tachy_wasapi_now(self) >= begin) {
                flags = AUDCLNT_BUFFERFLAGS_SILENT;
                fault = TACHY_SCHEDULE_CALLBACK_LATE;
                hr = E_FAIL;
            }
        } else {
            flags = AUDCLNT_BUFFERFLAGS_SILENT;
            if (self->observations) {
                fault = TACHY_SCHEDULE_CALLBACK_LATE;
                hr = E_FAIL;
            }
        }
        HRESULT released = IAudioRenderClient_ReleaseBuffer(render, frames, flags);
        if (SUCCEEDED(hr)) hr = released;
        if (FAILED(hr)) {
            self->fault = fault;
            for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
                TachyWasapiStimulus *slot = &self->slots[i];
                if (tachy_wasapi_active(slot) ||
                    (slot->state == TACHY_SCHEDULE_RENDERED && !slot->complete))
                    slot->state = fault;
            }
        } else {
            uint64_t complete = tachy_wasapi_now(self);
            for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
                TachyWasapiStimulus *slot = &self->slots[i];
                if (slot->state == TACHY_SCHEDULE_RENDERED && !slot->complete)
                    slot->complete = complete;
            }
        }
        ReleaseSRWLockExclusive(&self->lock);
        if (FAILED(hr)) break;
        submitted += frames;
        if (begin > now && !ready) {
            ready = 1;
            SetEvent(self->ready);
        }
    }

done:
    AcquireSRWLockExclusive(&self->lock);
    self->error = hr;
    if (FAILED(hr)) {
        self->fault = fault;
        for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
            if (tachy_wasapi_active(&self->slots[i])) self->slots[i].state = fault;
        }
    }
    ReleaseSRWLockExclusive(&self->lock);
    SetEvent(self->ready);
    if (started) IAudioClient_Stop(client);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (clock) IAudioClock_Release(clock);
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (events[1]) CloseHandle(events[1]);
    CoTaskMemFree(format);
    PyMem_RawFree(mix);
    if (device) IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return 0;
}

static void tachy_wasapi_shutdown(TachyScheduledOutput *self)
{
    self->closed = 1;
    if (self->thread) {
        SetEvent(self->stop);
        WaitForSingleObject(self->thread, INFINITE);
        CloseHandle(self->thread);
        self->thread = NULL;
    }
    for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
        TachyWasapiStimulus *slot = &self->slots[i];
        if (tachy_wasapi_active(slot)) slot->state = TACHY_SCHEDULE_CANCELLED;
        PyMem_RawFree(slot->samples);
        slot->samples = NULL;
    }
}

static void tachy_wasapi_dealloc(TachyScheduledOutput *self)
{
    tachy_wasapi_shutdown(self);
    if (self->stop) CloseHandle(self->stop);
    if (self->ready) CloseHandle(self->ready);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_wasapi_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"sample_rate", "channels", "device_id", NULL};
    int rate, channels;
    const char *endpoint;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "iiz", keywords, &rate, &channels, &endpoint))
        return NULL;
    if (rate <= 0 || channels <= 0 || channels > UINT16_MAX ||
        (endpoint && strncmp(endpoint, "output-", 7) != 0)) {
        PyErr_SetString(PyExc_ValueError, "expected a positive rate and channel count and a WASAPI output device id");
        return NULL;
    }
    TachyScheduledOutput *self = (TachyScheduledOutput *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    InitializeSRWLock(&self->lock);
    self->rate = rate;
    self->channels = channels;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    self->qpc_frequency = frequency.QuadPart;
    if (endpoint && !tachy_miniaudio_utf8_to_wasapi_id(
            self->endpoint, sizeof(self->endpoint) / sizeof(self->endpoint[0]), endpoint + 7)) {
        Py_DECREF(self);
        return NULL;
    }
    self->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    self->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!self->stop || !self->ready) {
        PyErr_SetFromWindowsErr(0);
        Py_DECREF(self);
        return NULL;
    }
    return (PyObject *)self;
}

static PyObject *tachy_wasapi_start(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed) {
        PyErr_SetString(PyExc_RuntimeError, "scheduled stream is closed");
        return NULL;
    }
    if (!self->thread) {
        self->thread = CreateThread(NULL, 0, tachy_wasapi_worker, self, 0, NULL);
        if (!self->thread) return PyErr_SetFromWindowsErr(0);
    }
    DWORD wait = WaitForSingleObject(self->ready, 3000);
    AcquireSRWLockShared(&self->lock);
    HRESULT hr = self->error;
    int fault = self->fault;
    ReleaseSRWLockShared(&self->lock);
    if (wait != WAIT_OBJECT_0 || fault) {
        PyErr_Format(PyExc_RuntimeError,
            "WASAPI scheduler could not start at %u Hz with %u channel(s) (HRESULT 0x%08lx). "
            "Use the endpoint's float32 shared mix rate and channel count, or mono.",
            self->rate, self->channels, (unsigned long)hr);
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *tachy_wasapi_schedule(TachyScheduledOutput *self, PyObject *args)
{
    PyObject *frames;
    unsigned long long target, lead;
    if (!PyArg_ParseTuple(args, "OKK", &frames, &target, &lead)) return NULL;
    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) < 0) return NULL;
    size_t stride = self->channels * sizeof(float);
    if (view.len <= 0 || (size_t)view.len % stride) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "expected complete interleaved float32 frames");
        return NULL;
    }
    size_t count = view.len / stride;
    float *copy = PyMem_RawMalloc(view.len);
    if (copy) memcpy(copy, view.buf, view.len);
    PyBuffer_Release(&view);
    if (!copy) return PyErr_NoMemory();
    for (size_t i = 0; i < count * self->channels; ++i) {
        if (!isfinite(copy[i])) {
            PyMem_RawFree(copy);
            PyErr_SetString(PyExc_ValueError, "stimulus samples must be finite");
            return NULL;
        }
    }
    AcquireSRWLockExclusive(&self->lock);
    uint64_t now = tachy_wasapi_now(self);
    TachyWasapiStimulus *slot = NULL;
    const char *problem = NULL;
    PyObject *exception = PyExc_ValueError;
    if (self->closed || self->fault || !self->observations) {
        problem = "stream is not running or has failed; reopen it";
        exception = PyExc_RuntimeError;
    } else if (target <= now || target - now < lead) {
        problem = "insufficient advance notice after copying stimulus data";
    } else {
        for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
            TachyWasapiStimulus *candidate = &self->slots[i];
            if (!tachy_wasapi_active(candidate) && candidate->end <= now &&
                (!slot || candidate->id < slot->id)) slot = candidate;
        }
        if (!slot) problem = "too many stimuli queued (32)";
    }
    uint64_t id = 0;
    if (slot) {
        float *old = slot->samples;
        TachyWasapiStimulus record = {0};
        record.samples = copy;
        record.frames = count;
        record.target = target;
        record.id = id = ++self->latest;
        record.state = TACHY_SCHEDULE_PENDING;
        *slot = record;
        copy = old;
    }
    ReleaseSRWLockExclusive(&self->lock);
    PyMem_RawFree(copy);
    if (problem) {
        PyErr_SetString(exception, problem);
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(id);
}

static PyObject *tachy_wasapi_result(TachyScheduledOutput *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id)) return NULL;
    TachyWasapiStimulus record = {0};
    AcquireSRWLockShared(&self->lock);
    if (!id) id = self->latest;
    for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
        if (id && self->slots[i].id == id) { record = self->slots[i]; break; }
    }
    ReleaseSRWLockShared(&self->lock);
    if (!record.id) {
        PyErr_SetString(PyExc_LookupError, "unknown stimulus id, or its record was reused");
        return NULL;
    }
    return Py_BuildValue("{s:i,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
        "state", record.state, "stimulus", record.id, "requested_ns", record.target,
        "predicted_onset_ns", record.onset, "render_callback_ns", record.callback,
        "render_complete_ns", record.complete, "predicted_end_ns", record.end,
        "rendered_frames", (uint64_t)record.rendered);
}

static PyObject *tachy_wasapi_status(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    unsigned int queued = 0;
    int state = TACHY_SCHEDULE_IDLE;
    AcquireSRWLockShared(&self->lock);
    for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
        queued += tachy_wasapi_active(&self->slots[i]);
        if (self->slots[i].id == self->latest) state = self->slots[i].state;
    }
    unsigned int buffer = self->buffer, period = self->period, channels = self->device_channels;
    uint64_t callbacks = self->callbacks, observations = self->observations;
    uint64_t delay = self->delay, max_delay = self->max_delay, latency = self->latency_ns;
    int mmcss = self->mmcss;
    if (self->fault) state = self->fault;
    ReleaseSRWLockShared(&self->lock);
    return Py_BuildValue("{s:i,s:I,s:I,s:I,s:I,s:I,s:s,s:s,s:s,s:K,s:K,s:d,s:d,s:d}",
        "state", state, "buffer_frames", buffer, "period_frames", period,
        "sample_rate", self->rate, "device_channels", channels, "queued", queued,
        "clock_source", "wasapi-audioclock-qpc", "device_format", "float32",
        "rt_method", mmcss ? "wasapi-mmcss-pro-audio" : "wasapi-event-thread",
        "callbacks", callbacks, "clock_observations", observations,
        "output_delay_us", delay / 1000.0, "output_delay_max_us", max_delay / 1000.0,
        "stream_latency_us", latency / 1000.0);
}

static PyObject *tachy_wasapi_cancel(TachyScheduledOutput *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id)) return NULL;
    AcquireSRWLockExclusive(&self->lock);
    for (int i = 0; i < TACHY_WASAPI_QUEUE; ++i) {
        TachyWasapiStimulus *slot = &self->slots[i];
        if ((!id || slot->id == id) && tachy_wasapi_active(slot))
            slot->state = TACHY_SCHEDULE_CANCELLED;
    }
    ReleaseSRWLockExclusive(&self->lock);
    Py_RETURN_NONE;
}

static PyObject *tachy_wasapi_close(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    tachy_wasapi_shutdown(self);
    Py_RETURN_NONE;
}

static PyMethodDef tachy_wasapi_methods[] = {
    {"start", (PyCFunction)tachy_wasapi_start, METH_NOARGS, NULL},
    {"schedule", (PyCFunction)tachy_wasapi_schedule, METH_VARARGS, NULL},
    {"result", (PyCFunction)tachy_wasapi_result, METH_VARARGS, NULL},
    {"status", (PyCFunction)tachy_wasapi_status, METH_NOARGS, NULL},
    {"cancel", (PyCFunction)tachy_wasapi_cancel, METH_VARARGS, NULL},
    {"close", (PyCFunction)tachy_wasapi_close, METH_NOARGS, NULL},
    {NULL}
};

static PyTypeObject TachyScheduledOutputType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.ScheduledOutputStream",
    .tp_basicsize = sizeof(TachyScheduledOutput),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = tachy_wasapi_new,
    .tp_dealloc = (destructor)tachy_wasapi_dealloc,
    .tp_methods = tachy_wasapi_methods
};
