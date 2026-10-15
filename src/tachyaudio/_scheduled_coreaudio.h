#include <stdatomic.h>
#include <math.h>

enum {
    TACHY_SCHEDULE_IDLE,
    TACHY_SCHEDULE_PREPARING,
    TACHY_SCHEDULE_PENDING,
    TACHY_SCHEDULE_PLAYING,
    TACHY_SCHEDULE_RENDERED,
    TACHY_SCHEDULE_CANCELLED,
    TACHY_SCHEDULE_MISSED,
    TACHY_SCHEDULE_CLOCK_ERROR,
    TACHY_SCHEDULE_DISCONTINUITY,
    TACHY_SCHEDULE_CALLBACK_LATE,
    TACHY_SCHEDULE_FORMAT_ERROR
};

typedef struct {
    PyObject_HEAD
    AudioObjectID device;
    AudioDeviceIOProcID proc;
    mach_timebase_info_data_t timebase;
    double sample_rate;
    UInt32 channels;
    UInt32 device_channels;
    UInt32 buffer_frames;
    int running;
    int busy;
    int closed;
    int listener_installed;
    float *samples;
    size_t frame_count;
    size_t position;
    UInt64 requested_ns;
    UInt64 predicted_onset_ns;
    UInt64 render_callback_ns;
    UInt64 render_complete_ns;
    UInt64 predicted_end_ns;
    UInt64 decision_buffer_ns;
    double expected_sample_time;
    atomic_int state;
    atomic_int cancel_requested;
    atomic_int device_changed;
    atomic_ullong callbacks;
    atomic_ullong rendered_frames;
} TachyScheduledOutput;

static OSStatus tachy_scheduled_device_changed(AudioObjectID device, UInt32 count,
    const AudioObjectPropertyAddress addresses[], void *context)
{
    (void)device;
    (void)count;
    (void)addresses;
    TachyScheduledOutput *self = context;
    atomic_store_explicit(&self->device_changed, 1, memory_order_release);
    return noErr;
}

static void tachy_scheduled_remove_listener(TachyScheduledOutput *self)
{
    if (self->listener_installed) {
        AudioObjectPropertyAddress address = {
            kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectRemovePropertyListener(self->device, &address,
                                         tachy_scheduled_device_changed, self);
        self->listener_installed = 0;
    }
}

static UInt64 tachy_scheduled_ns(TachyScheduledOutput *self, UInt64 ticks)
{
    return (ticks / self->timebase.denom) * self->timebase.numer +
        ((ticks % self->timebase.denom) * self->timebase.numer) / self->timebase.denom;
}

static long long tachy_schedule_offset(UInt64 target, UInt64 beginning, double rate)
{
    if (target < beginning) {
        return (double)(beginning - target) < 1000000000.0 / rate ? 0 : -1;
    }
    double offset = ceil(((double)(target - beginning) / 1000000000.0) * rate);
    if (offset > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (long long)offset;
}

static OSStatus tachy_scheduled_render(
    AudioObjectID device, const AudioTimeStamp *now,
    const AudioBufferList *input, const AudioTimeStamp *input_time,
    AudioBufferList *output, const AudioTimeStamp *output_time, void *context)
{
    (void)device;
    (void)now;
    (void)input;
    (void)input_time;
    TachyScheduledOutput *self = context;
    atomic_fetch_add_explicit(&self->callbacks, 1, memory_order_relaxed);
    for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
        if (output->mBuffers[index].mData != NULL) {
            memset(output->mBuffers[index].mData, 0, output->mBuffers[index].mDataByteSize);
        }
    }
    int state = atomic_load_explicit(&self->state, memory_order_acquire);
    if (state != TACHY_SCHEDULE_PENDING && state != TACHY_SCHEDULE_PLAYING) {
        return noErr;
    }
    if (atomic_load_explicit(&self->device_changed, memory_order_acquire)) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_DISCONTINUITY, memory_order_release);
        return noErr;
    }
    if (atomic_load_explicit(&self->cancel_requested, memory_order_acquire)) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_CANCELLED, memory_order_release);
        return noErr;
    }
    AudioTimeStampFlags required = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
    if ((output_time->mFlags & required) != required) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_CLOCK_ERROR, memory_order_release);
        return noErr;
    }
    UInt32 frame_count = 0;
    UInt32 channels = 0;
    for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
        AudioBuffer *buffer = &output->mBuffers[index];
        if (buffer->mNumberChannels == 0 || buffer->mData == NULL ||
            buffer->mDataByteSize % (buffer->mNumberChannels * sizeof(float)) != 0) {
            atomic_store_explicit(&self->state, TACHY_SCHEDULE_FORMAT_ERROR, memory_order_release);
            return noErr;
        }
        UInt32 frames = buffer->mDataByteSize / (buffer->mNumberChannels * sizeof(float));
        if (index == 0) {
            frame_count = frames;
        } else if (frames != frame_count) {
            atomic_store_explicit(&self->state, TACHY_SCHEDULE_FORMAT_ERROR, memory_order_release);
            return noErr;
        }
        channels += buffer->mNumberChannels;
    }
    if (frame_count == 0 || channels != self->device_channels) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_FORMAT_ERROR, memory_order_release);
        return noErr;
    }
    UInt64 beginning = tachy_scheduled_ns(self, output_time->mHostTime);
    UInt32 offset = 0;
    if (state == TACHY_SCHEDULE_PENDING) {
        long long wanted_offset = tachy_schedule_offset(self->requested_ns, beginning, self->sample_rate);
        if (wanted_offset < 0) {
            self->decision_buffer_ns = beginning;
            atomic_store_explicit(&self->state, TACHY_SCHEDULE_MISSED, memory_order_release);
            return noErr;
        }
        if ((UInt64)wanted_offset >= frame_count) {
            return noErr;
        }
        offset = (UInt32)wanted_offset;
        self->decision_buffer_ns = beginning;
        self->predicted_onset_ns = beginning + (UInt64)llround(offset * 1000000000.0 / self->sample_rate);
        self->render_callback_ns = tachy_scheduled_ns(self, mach_absolute_time());
        if (self->render_callback_ns >= self->predicted_onset_ns) {
            atomic_store_explicit(&self->state, TACHY_SCHEDULE_CALLBACK_LATE, memory_order_release);
            return noErr;
        }
    } else if (fabs(output_time->mSampleTime - self->expected_sample_time) > 0.5) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_DISCONTINUITY, memory_order_release);
        return noErr;
    }
    if (state == TACHY_SCHEDULE_PLAYING &&
        tachy_scheduled_ns(self, mach_absolute_time()) >= beginning) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_CALLBACK_LATE, memory_order_release);
        return noErr;
    }
    size_t count = self->frame_count - self->position;
    if (count > frame_count - offset) {
        count = frame_count - offset;
    }
    UInt32 channel_base = 0;
    for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
        AudioBuffer *buffer = &output->mBuffers[index];
        float *destination = buffer->mData;
        for (size_t frame = 0; frame < count; frame++) {
            for (UInt32 channel = 0; channel < buffer->mNumberChannels; channel++) {
                UInt32 source_channel = self->channels == 1 ? 0 : channel_base + channel;
                destination[(offset + frame) * buffer->mNumberChannels + channel] =
                    self->samples[(self->position + frame) * self->channels + source_channel];
            }
        }
        channel_base += buffer->mNumberChannels;
    }
    UInt64 render_complete_ns = tachy_scheduled_ns(self, mach_absolute_time());
    UInt64 first_sample_ns = beginning + (UInt64)llround(offset * 1e9 / self->sample_rate);
    if (render_complete_ns >= first_sample_ns) {
        for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
            memset(output->mBuffers[index].mData, 0, output->mBuffers[index].mDataByteSize);
        }
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_CALLBACK_LATE, memory_order_release);
        return noErr;
    }
    if (state == TACHY_SCHEDULE_PENDING) {
        self->render_complete_ns = render_complete_ns;
    }
    self->position += count;
    self->expected_sample_time = output_time->mSampleTime + frame_count;
    atomic_store_explicit(&self->rendered_frames, self->position, memory_order_relaxed);
    if (self->position == self->frame_count) {
        self->predicted_end_ns = beginning +
            (UInt64)llround((offset + count) * 1000000000.0 / self->sample_rate);
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_RENDERED, memory_order_release);
    } else if (state == TACHY_SCHEDULE_PENDING) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_PLAYING, memory_order_release);
    }
    return noErr;
}

static void tachy_scheduled_dealloc(TachyScheduledOutput *self)
{
    tachy_scheduled_remove_listener(self);
    if (self->proc != NULL) {
        if (self->running) {
            AudioDeviceStop(self->device, self->proc);
        }
        AudioDeviceDestroyIOProcID(self->device, self->proc);
    }
    PyMem_RawFree(self->samples);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_scheduled_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"sample_rate", "channels", "device_id", NULL};
    int sample_rate = 48000;
    int channels = 2;
    const char *device_id = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iiz", keywords,
                                    &sample_rate, &channels, &device_id)) {
        return NULL;
    }
    if (sample_rate < 1 || channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample rate and channels must be positive");
        return NULL;
    }
    TachyScheduledOutput *self = (TachyScheduledOutput *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    atomic_init(&self->state, TACHY_SCHEDULE_IDLE);
    atomic_init(&self->cancel_requested, 0);
    atomic_init(&self->device_changed, 0);
    atomic_init(&self->callbacks, 0);
    atomic_init(&self->rendered_frames, 0);
    if (!atomic_is_lock_free(&self->state) || !atomic_is_lock_free(&self->callbacks)) {
        PyErr_SetString(PyExc_RuntimeError, "native scheduling requires lock-free atomics");
        Py_DECREF(self);
        return NULL;
    }
    mach_timebase_info(&self->timebase);
    self->sample_rate = sample_rate;
    self->channels = channels;
    self->device = device_id == NULL ? tachy_get_default_device(kAudioHardwarePropertyDefaultOutputDevice) :
        tachy_find_device_by_uid(device_id);
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus result = AudioObjectGetPropertyDataSize(self->device, &address, 0, NULL, &size);
    if (result != noErr || size == 0) {
        PyErr_SetString(PyExc_RuntimeError, "could not inspect output device streams");
        Py_DECREF(self);
        return NULL;
    }
    AudioStreamID *streams = PyMem_RawMalloc(size);
    if (streams == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }
    result = AudioObjectGetPropertyData(self->device, &address, 0, NULL, &size, streams);
    if (result == noErr) {
        UInt32 stream_count = size / sizeof(AudioStreamID);
        for (UInt32 index = 0; index < stream_count; index++) {
            AudioStreamBasicDescription format;
            UInt32 format_size = sizeof(format);
            AudioObjectPropertyAddress format_address = {
                kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain
            };
            result = AudioObjectGetPropertyData(streams[index], &format_address, 0, NULL,
                                               &format_size, &format);
            if (result != noErr || format.mFormatID != kAudioFormatLinearPCM ||
                !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
                (format.mFormatFlags & kAudioFormatFlagIsBigEndian) ||
                format.mBitsPerChannel != 32 || format.mSampleRate != sample_rate) {
                result = kAudio_ParamError;
                break;
            }
            self->device_channels += format.mChannelsPerFrame;
        }
    }
    PyMem_RawFree(streams);
    if (result != noErr || (channels != 1 && (UInt32)channels != self->device_channels)) {
        PyErr_SetString(PyExc_ValueError,
            "native scheduling requires float32 device format at the requested sample rate; "
            "use mono or the device's full output channel count");
        Py_DECREF(self);
        return NULL;
    }
    address.mSelector = kAudioDevicePropertyBufferFrameSize;
    address.mScope = kAudioObjectPropertyScopeGlobal;
    size = sizeof(self->buffer_frames);
    result = AudioObjectGetPropertyData(self->device, &address, 0, NULL, &size, &self->buffer_frames);
    if (result == noErr) {
        result = AudioDeviceCreateIOProcID(self->device, tachy_scheduled_render, self, &self->proc);
    }
    if (result == noErr) {
        address.mSelector = kAudioDevicePropertyNominalSampleRate;
        result = AudioObjectAddPropertyListener(self->device, &address,
                                               tachy_scheduled_device_changed, self);
        if (result == noErr) {
            self->listener_installed = 1;
        }
    }
    if (result != noErr) {
        PyErr_Format(PyExc_RuntimeError, "could not create device render callback (%d)", (int)result);
        Py_DECREF(self);
        return NULL;
    }
    return (PyObject *)self;
}

static int tachy_scheduled_check(TachyScheduledOutput *self)
{
    if (self->closed || self->busy) {
        PyErr_SetString(PyExc_RuntimeError, "scheduled stream is closed or changing state");
        return 0;
    }
    return 1;
}

static PyObject *tachy_scheduled_start(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    if (!tachy_scheduled_check(self)) {
        return NULL;
    }
    if (!self->running) {
        self->busy = 1;
        OSStatus result;
        Py_BEGIN_ALLOW_THREADS
        result = AudioDeviceStart(self->device, self->proc);
        Py_END_ALLOW_THREADS
        self->busy = 0;
        if (result != noErr) {
            PyErr_Format(PyExc_RuntimeError, "could not start audio device (%d)", (int)result);
            return NULL;
        }
        self->running = 1;
    }
    Py_RETURN_NONE;
}

static PyObject *tachy_scheduled_schedule(TachyScheduledOutput *self, PyObject *args)
{
    PyObject *frames;
    unsigned long long when_ns;
    unsigned long long minimum_lead_ns = 0;
    if (!PyArg_ParseTuple(args, "OK|K", &frames, &when_ns, &minimum_lead_ns) ||
        !tachy_scheduled_check(self)) {
        return NULL;
    }
    int state = atomic_load_explicit(&self->state, memory_order_acquire);
    if (!self->running || state == TACHY_SCHEDULE_PENDING || state == TACHY_SCHEDULE_PLAYING) {
        PyErr_SetString(PyExc_RuntimeError, "start the stream and finish its previous stimulus first");
        return NULL;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) != 0) {
        return NULL;
    }
    size_t frame_bytes = self->channels * sizeof(float);
    if (view.len <= 0 || (size_t)view.len % frame_bytes != 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "expected complete interleaved float32 frames");
        return NULL;
    }
    float *samples = PyMem_RawMalloc(view.len);
    if (samples == NULL) {
        PyBuffer_Release(&view);
        return PyErr_NoMemory();
    }
    memcpy(samples, view.buf, view.len);
    size_t frame_count = (size_t)view.len / frame_bytes;
    PyBuffer_Release(&view);
    UInt64 now_ns = tachy_scheduled_ns(self, mach_absolute_time());
    if (when_ns <= now_ns || when_ns - now_ns < minimum_lead_ns) {
        PyMem_RawFree(samples);
        PyErr_SetString(PyExc_ValueError, "insufficient advance notice after copying stimulus data");
        return NULL;
    }
    atomic_store_explicit(&self->state, TACHY_SCHEDULE_PREPARING, memory_order_release);
    PyMem_RawFree(self->samples);
    self->samples = samples;
    self->frame_count = frame_count;
    self->position = 0;
    self->requested_ns = when_ns;
    self->predicted_onset_ns = 0;
    self->render_callback_ns = 0;
    self->render_complete_ns = 0;
    self->predicted_end_ns = 0;
    self->decision_buffer_ns = 0;
    atomic_store_explicit(&self->rendered_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&self->cancel_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&self->state, TACHY_SCHEDULE_PENDING, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *tachy_scheduled_status(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    int state = atomic_load_explicit(&self->state, memory_order_acquire);
    int published = state >= TACHY_SCHEDULE_PLAYING;
    return Py_BuildValue("{s:i,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:I,s:d}",
        "state", state, "requested_ns", self->requested_ns,
        "predicted_onset_ns", published ? self->predicted_onset_ns : 0,
        "render_callback_ns", published ? self->render_callback_ns : 0,
        "render_complete_ns", published ? self->render_complete_ns : 0,
        "predicted_end_ns", state == TACHY_SCHEDULE_RENDERED ? self->predicted_end_ns : 0,
        "decision_buffer_ns", published ? self->decision_buffer_ns : 0,
        "rendered_frames", atomic_load_explicit(&self->rendered_frames, memory_order_relaxed),
        "callbacks", atomic_load_explicit(&self->callbacks, memory_order_relaxed),
        "buffer_frames", self->buffer_frames, "sample_rate", self->sample_rate);
}

static PyObject *tachy_scheduled_cancel(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    atomic_store_explicit(&self->cancel_requested, 1, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *tachy_scheduled_close(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed) {
        Py_RETURN_NONE;
    }
    if (!tachy_scheduled_check(self)) {
        return NULL;
    }
    self->busy = 1;
    OSStatus result = noErr;
    Py_BEGIN_ALLOW_THREADS
    if (self->running) {
        result = AudioDeviceStop(self->device, self->proc);
    }
    Py_END_ALLOW_THREADS
    self->busy = 0;
    if (result != noErr) {
        PyErr_Format(PyExc_RuntimeError, "could not stop audio device (%d)", (int)result);
        return NULL;
    }
    self->running = 0;
    tachy_scheduled_remove_listener(self);
    AudioDeviceDestroyIOProcID(self->device, self->proc);
    self->proc = NULL;
    self->closed = 1;
    int state = atomic_load_explicit(&self->state, memory_order_acquire);
    if (state == TACHY_SCHEDULE_PENDING || state == TACHY_SCHEDULE_PLAYING) {
        atomic_store_explicit(&self->state, TACHY_SCHEDULE_CANCELLED, memory_order_release);
    }
    Py_RETURN_NONE;
}

static PyMethodDef tachy_scheduled_methods[] = {
    {"start", (PyCFunction)tachy_scheduled_start, METH_NOARGS, "Start device callbacks."},
    {"schedule", (PyCFunction)tachy_scheduled_schedule, METH_VARARGS, "Submit a future stimulus."},
    {"status", (PyCFunction)tachy_scheduled_status, METH_NOARGS, "Read device-clock timing estimates."},
    {"cancel", (PyCFunction)tachy_scheduled_cancel, METH_NOARGS, "Cancel at the next callback."},
    {"close", (PyCFunction)tachy_scheduled_close, METH_NOARGS, "Release the output device."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyScheduledOutputType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.ScheduledOutputStream",
    .tp_basicsize = sizeof(TachyScheduledOutput),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Experimental device-clock scheduled macOS output.",
    .tp_new = tachy_scheduled_new,
    .tp_dealloc = (destructor)tachy_scheduled_dealloc,
    .tp_methods = tachy_scheduled_methods,
};

static PyObject *tachy_scheduled_render_test(PyObject *module, PyObject *args)
{
    (void)module;
    PyObject *samples;
    int channels;
    unsigned long long requested_ns, beginning_ns;
    unsigned int frames, callbacks;
    int gap = 0;
    if (!PyArg_ParseTuple(args, "OiKKII|p", &samples, &channels, &requested_ns,
                          &beginning_ns, &frames, &callbacks, &gap)) {
        return NULL;
    }
    if (channels < 1 || channels > 8 || frames < 1 || frames > 4096 || callbacks > 64) {
        PyErr_SetString(PyExc_ValueError, "invalid renderer test dimensions");
        return NULL;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(samples, &view, PyBUF_CONTIG_RO) != 0) {
        return NULL;
    }
    if (view.len <= 0 || view.len % (channels * sizeof(float)) != 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "invalid renderer test samples");
        return NULL;
    }
    TachyScheduledOutput self = {0};
    mach_timebase_info(&self.timebase);
    self.channels = channels;
    self.device_channels = channels;
    self.sample_rate = 48000;
    self.samples = view.buf;
    self.frame_count = view.len / (channels * sizeof(float));
    self.requested_ns = requested_ns;
    atomic_init(&self.state, TACHY_SCHEDULE_PENDING);
    atomic_init(&self.cancel_requested, 0);
    atomic_init(&self.device_changed, 0);
    atomic_init(&self.callbacks, 0);
    atomic_init(&self.rendered_frames, 0);
    PyObject *buffers = PyList_New(0);
    if (buffers == NULL) {
        PyBuffer_Release(&view);
        return NULL;
    }
    for (UInt32 index = 0; index < callbacks; index++) {
        PyObject *data = PyBytes_FromStringAndSize(NULL, frames * channels * sizeof(float));
        if (data == NULL) {
            Py_DECREF(buffers);
            PyBuffer_Release(&view);
            return NULL;
        }
        AudioBufferList output = {0};
        output.mNumberBuffers = 1;
        output.mBuffers[0].mNumberChannels = channels;
        output.mBuffers[0].mDataByteSize = frames * channels * sizeof(float);
        output.mBuffers[0].mData = PyBytes_AS_STRING(data);
        AudioTimeStamp stamp = {0};
        UInt64 host_ns = beginning_ns + (UInt64)llround(index * frames * 1e9 / 48000.0);
        stamp.mHostTime = (host_ns / self.timebase.numer) * self.timebase.denom +
            ((host_ns % self.timebase.numer) * self.timebase.denom) / self.timebase.numer;
        stamp.mSampleTime = index * frames + (index > 0 && gap ? 10 : 0);
        stamp.mFlags = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
        tachy_scheduled_render(0, &stamp, NULL, NULL, &output, &stamp, &self);
        int appended = PyList_Append(buffers, data);
        Py_DECREF(data);
        if (appended < 0) {
            Py_DECREF(buffers);
            PyBuffer_Release(&view);
            return NULL;
        }
    }
    PyBuffer_Release(&view);
    PyObject *status = tachy_scheduled_status(&self, NULL);
    if (status == NULL) {
        Py_DECREF(buffers);
        return NULL;
    }
    return Py_BuildValue("NN", buffers, status);
}
