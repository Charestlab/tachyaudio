#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TACHY_OUTPUT_BUFFER_COUNT 3
#define TACHY_INPUT_BUFFER_COUNT 3
#define TACHY_DEFAULT_BUFFER_MS 10
#define TACHY_RING_SECONDS 1

static size_t tachy_min_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

static void tachy_ring_copy_in_raw(
    uint8_t *ring,
    size_t ring_capacity,
    size_t *ring_write,
    size_t *ring_size,
    const uint8_t *source,
    size_t byte_count)
{
    size_t first = tachy_min_size(byte_count, ring_capacity - *ring_write);
    memcpy(ring + *ring_write, source, first);
    memcpy(ring, source + first, byte_count - first);
    *ring_write = (*ring_write + byte_count) % ring_capacity;
    *ring_size += byte_count;
}

static size_t tachy_ring_copy_out_raw(
    uint8_t *ring,
    size_t ring_capacity,
    size_t *ring_read,
    size_t *ring_size,
    uint8_t *target,
    size_t byte_count)
{
    size_t copied = tachy_min_size(byte_count, *ring_size);
    size_t first = tachy_min_size(copied, ring_capacity - *ring_read);

    memcpy(target, ring + *ring_read, first);
    memcpy(target + first, ring, copied - first);
    *ring_read = (*ring_read + copied) % ring_capacity;
    *ring_size -= copied;
    return copied;
}

static PyObject *tachy_build_stream_stats(
    unsigned long long frames_processed,
    unsigned int underruns,
    unsigned int overruns,
    unsigned long long queued_frames,
    double queued_latency,
    double hardware_latency,
    int has_hardware_latency,
    unsigned int buffer_size)
{
    double estimated_latency = queued_latency;
    PyObject *hardware_latency_object = Py_None;
    if (has_hardware_latency) {
        estimated_latency += hardware_latency;
        hardware_latency_object = PyFloat_FromDouble(hardware_latency);
        if (hardware_latency_object == NULL) {
            return NULL;
        }
    } else {
        Py_INCREF(Py_None);
    }

    PyObject *stats = NULL;
    stats = Py_BuildValue(
        "{s:K,s:I,s:I,s:d,s:O,s:K,s:d,s:I}",
        "frames_processed", frames_processed,
        "underruns", underruns,
        "overruns", overruns,
        "estimated_latency", estimated_latency,
        "hardware_latency", hardware_latency_object,
        "queued_frames", queued_frames,
        "queued_latency", queued_latency,
        "buffer_size", buffer_size
    );
    Py_DECREF(hardware_latency_object);
    return stats;
}

#ifdef __linux__
static PyObject *tachy_build_stream_stats_without_hardware_latency(
    unsigned long long frames_processed,
    unsigned int underruns,
    unsigned int overruns,
    unsigned long long queued_frames,
    double queued_latency,
    unsigned int buffer_size)
{
    return Py_BuildValue(
        "{s:K,s:I,s:I,s:d,s:O,s:K,s:d,s:I}",
        "frames_processed", frames_processed,
        "underruns", underruns,
        "overruns", overruns,
        "estimated_latency", queued_latency,
        "hardware_latency", Py_None,
        "queued_frames", queued_frames,
        "queued_latency", queued_latency,
        "buffer_size", buffer_size
    );
}
#endif

#ifdef __linux__
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"
#endif

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __APPLE__

typedef struct {
    PyObject_HEAD
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[TACHY_OUTPUT_BUFFER_COUNT];
    AudioStreamBasicDescription format;
    UInt32 bytes_per_frame;
    UInt32 buffer_byte_size;
    UInt64 frames_processed;
    UInt32 pending_buffers;
    UInt32 underruns;
    UInt32 overruns;
    double hardware_latency;
    uint8_t *ring;
    size_t ring_capacity;
    size_t ring_read;
    size_t ring_write;
    size_t ring_size;
    pthread_mutex_t lock;
    int lock_initialized;
    int started;
    int closed;
    int draining;
} TachyOutputStream;

static PyTypeObject TachyOutputStreamType;

typedef struct {
    PyObject_HEAD
    AudioQueueRef queue;
    AudioQueueBufferRef buffers[TACHY_INPUT_BUFFER_COUNT];
    AudioStreamBasicDescription format;
    UInt32 bytes_per_frame;
    UInt32 buffer_byte_size;
    UInt64 frames_processed;
    UInt32 underruns;
    UInt32 overruns;
    double hardware_latency;
    uint8_t *ring;
    size_t ring_capacity;
    size_t ring_read;
    size_t ring_write;
    size_t ring_size;
    pthread_mutex_t lock;
    int lock_initialized;
    int started;
    int closed;
} TachyInputStream;

static PyTypeObject TachyInputStreamType;

static AudioObjectID tachy_get_default_device(AudioObjectPropertySelector selector);
static AudioObjectID tachy_find_device_by_uid(const char *device_uid);
static double tachy_get_coreaudio_hardware_latency(
    AudioObjectID device_id,
    AudioObjectPropertyScope scope,
    double sample_rate);

static void tachy_ring_copy_in(TachyOutputStream *stream, const uint8_t *source, size_t byte_count)
{
    tachy_ring_copy_in_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_write,
        &stream->ring_size,
        source,
        byte_count);
}

static void tachy_input_ring_copy_in(TachyInputStream *stream, const uint8_t *source, size_t byte_count)
{
    tachy_ring_copy_in_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_write,
        &stream->ring_size,
        source,
        byte_count);
}

static size_t tachy_input_ring_copy_out(TachyInputStream *stream, uint8_t *target, size_t byte_count)
{
    return tachy_ring_copy_out_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_read,
        &stream->ring_size,
        target,
        byte_count);
}

static size_t tachy_ring_copy_out(TachyOutputStream *stream, uint8_t *target, size_t byte_count)
{
    return tachy_ring_copy_out_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_read,
        &stream->ring_size,
        target,
        byte_count);
}

static void tachy_fill_output_buffer(TachyOutputStream *stream, AudioQueueBufferRef buffer)
{
    pthread_mutex_lock(&stream->lock);
    size_t copied = tachy_ring_copy_out(stream, (uint8_t *)buffer->mAudioData, stream->buffer_byte_size);
    if (copied < stream->buffer_byte_size) {
        memset((uint8_t *)buffer->mAudioData + copied, 0, stream->buffer_byte_size - copied);
        if (!(stream->draining && copied > 0)) {
            stream->underruns += 1;
        }
    }
    stream->frames_processed += stream->buffer_byte_size / stream->bytes_per_frame;
    pthread_mutex_unlock(&stream->lock);

    buffer->mAudioDataByteSize = stream->buffer_byte_size;
}

static int tachy_enqueue_output_buffer(TachyOutputStream *stream, AudioQueueBufferRef buffer)
{
    tachy_fill_output_buffer(stream, buffer);

    OSStatus status = AudioQueueEnqueueBuffer(stream->queue, buffer, 0, NULL);
    if (status != noErr) {
        return 0;
    }

    pthread_mutex_lock(&stream->lock);
    stream->pending_buffers += 1;
    pthread_mutex_unlock(&stream->lock);
    return 1;
}

static void tachy_output_callback(void *user_data, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
    (void)queue;
    TachyOutputStream *stream = (TachyOutputStream *)user_data;

    pthread_mutex_lock(&stream->lock);
    if (stream->pending_buffers > 0) {
        stream->pending_buffers -= 1;
    }
    int should_continue = stream->started && !stream->closed;
    if (should_continue && stream->draining && stream->ring_size == 0) {
        stream->started = 0;
        should_continue = 0;
    }
    pthread_mutex_unlock(&stream->lock);

    if (should_continue) {
        (void)tachy_enqueue_output_buffer(stream, buffer);
    }
}

static int tachy_set_output_device(AudioQueueRef queue, const char *device_uid)
{
    if (device_uid == NULL || device_uid[0] == '\0') {
        return 1;
    }

    CFStringRef uid = CFStringCreateWithCString(NULL, device_uid, kCFStringEncodingUTF8);
    if (uid == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create Core Audio device uid");
        return 0;
    }

    OSStatus status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
    CFRelease(uid);

    if (status != noErr) {
        PyErr_SetString(PyExc_RuntimeError, "failed to set Core Audio output device");
        return 0;
    }

    return 1;
}

static int tachy_set_input_device(AudioQueueRef queue, const char *device_uid)
{
    if (device_uid == NULL || device_uid[0] == '\0') {
        return 1;
    }

    CFStringRef uid = CFStringCreateWithCString(NULL, device_uid, kCFStringEncodingUTF8);
    if (uid == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create Core Audio device uid");
        return 0;
    }

    OSStatus status = AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
    CFRelease(uid);

    if (status != noErr) {
        PyErr_SetString(PyExc_RuntimeError, "failed to set Core Audio input device");
        return 0;
    }

    return 1;
}

static PyObject *tachy_output_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "sample_rate",
        "channels",
        "block_size",
        "device_id",
        "latency",
        NULL
    };
    int sample_rate = 0;
    int channels = 0;
    int block_size = 0;
    const char *device_id = NULL;
    double latency = 0.0;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "iiizd",
            keywords,
            &sample_rate,
            &channels,
            &block_size,
            &device_id,
            &latency)) {
        return NULL;
    }

    if (sample_rate < 1 || channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate and channels must be positive");
        return NULL;
    }

    TachyOutputStream *self = (TachyOutputStream *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->bytes_per_frame = (UInt32)channels * sizeof(float);
    UInt32 buffer_frames = (UInt32)(sample_rate * TACHY_DEFAULT_BUFFER_MS / 1000);
    if (block_size > 0) {
        buffer_frames = (UInt32)block_size;
    }
    if (latency > 0.0) {
        UInt32 latency_frames = (UInt32)(((double)sample_rate * latency) / TACHY_OUTPUT_BUFFER_COUNT);
        if (latency_frames > 0) {
            buffer_frames = latency_frames;
        }
    }
    if (buffer_frames < 64) {
        buffer_frames = 64;
    }
    self->buffer_byte_size = buffer_frames * self->bytes_per_frame;
    self->frames_processed = 0;
    self->pending_buffers = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->hardware_latency = 0.0;
    self->ring = NULL;
    self->ring_capacity = (size_t)sample_rate * TACHY_RING_SECONDS * self->bytes_per_frame;
    if (self->ring_capacity < (size_t)self->buffer_byte_size * TACHY_OUTPUT_BUFFER_COUNT * 2) {
        self->ring_capacity = (size_t)self->buffer_byte_size * TACHY_OUTPUT_BUFFER_COUNT * 2;
    }
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    self->lock_initialized = 0;
    self->started = 0;
    self->closed = 0;
    self->draining = 0;
    for (UInt32 index = 0; index < TACHY_OUTPUT_BUFFER_COUNT; index++) {
        self->buffers[index] = NULL;
    }

    self->ring = (uint8_t *)PyMem_RawMalloc(self->ring_capacity);
    if (self->ring == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize output stream lock");
        return NULL;
    }
    self->lock_initialized = 1;

    self->format.mSampleRate = (Float64)sample_rate;
    self->format.mFormatID = kAudioFormatLinearPCM;
    self->format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    self->format.mBytesPerPacket = self->bytes_per_frame;
    self->format.mFramesPerPacket = 1;
    self->format.mBytesPerFrame = self->bytes_per_frame;
    self->format.mChannelsPerFrame = (UInt32)channels;
    self->format.mBitsPerChannel = 8 * sizeof(float);
    self->format.mReserved = 0;

    OSStatus status = AudioQueueNewOutput(
        &self->format,
        tachy_output_callback,
        self,
        NULL,
        NULL,
        0,
        &self->queue);

    if (status != noErr) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to create Core Audio output queue");
        return NULL;
    }

    if (!tachy_set_output_device(self->queue, device_id)) {
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
        Py_DECREF(self);
        return NULL;
    }

    AudioObjectID output_device = device_id == NULL || device_id[0] == '\0'
        ? tachy_get_default_device(kAudioHardwarePropertyDefaultOutputDevice)
        : tachy_find_device_by_uid(device_id);
    self->hardware_latency = tachy_get_coreaudio_hardware_latency(
        output_device,
        kAudioDevicePropertyScopeOutput,
        (double)sample_rate);

    for (UInt32 index = 0; index < TACHY_OUTPUT_BUFFER_COUNT; index++) {
        status = AudioQueueAllocateBuffer(self->queue, self->buffer_byte_size, &self->buffers[index]);
        if (status != noErr || self->buffers[index] == NULL) {
            AudioQueueDispose(self->queue, true);
            self->queue = NULL;
            Py_DECREF(self);
            PyErr_SetString(PyExc_RuntimeError, "failed to allocate Core Audio output buffers");
            return NULL;
        }
    }

    return (PyObject *)self;
}

static void tachy_output_dealloc(TachyOutputStream *self)
{
    if (!self->closed && self->queue != NULL) {
        pthread_mutex_lock(&self->lock);
        self->closed = 1;
        self->started = 0;
        self->pending_buffers = 0;
        pthread_mutex_unlock(&self->lock);
        AudioQueueStop(self->queue, true);
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
    }
    if (self->lock_initialized) {
        pthread_mutex_destroy(&self->lock);
        self->lock_initialized = 0;
    }
    if (self->ring != NULL) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_output_start(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    if (self->started) {
        Py_RETURN_NONE;
    }

    pthread_mutex_lock(&self->lock);
    self->started = 1;
    self->pending_buffers = 0;
    self->draining = 0;
    pthread_mutex_unlock(&self->lock);

    for (UInt32 index = 0; index < TACHY_OUTPUT_BUFFER_COUNT; index++) {
        if (!tachy_enqueue_output_buffer(self, self->buffers[index])) {
            pthread_mutex_lock(&self->lock);
            self->started = 0;
            pthread_mutex_unlock(&self->lock);
            PyErr_SetString(PyExc_RuntimeError, "failed to enqueue Core Audio output buffer");
            return NULL;
        }
    }

    OSStatus status = AudioQueueStart(self->queue, NULL);
    if (status != noErr) {
        pthread_mutex_lock(&self->lock);
        self->started = 0;
        pthread_mutex_unlock(&self->lock);
        PyErr_SetString(PyExc_RuntimeError, "failed to start Core Audio output queue");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_output_stop(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->started = 0;
    self->pending_buffers = 0;
    self->draining = 0;
    pthread_mutex_unlock(&self->lock);

    OSStatus status = AudioQueueStop(self->queue, false);
    if (status != noErr) {
        PyErr_SetString(PyExc_RuntimeError, "failed to stop Core Audio output queue");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_output_drain(TachyOutputStream *self, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"timeout", NULL};
    double timeout = -1.0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|d", keywords, &timeout)) {
        return NULL;
    }

    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    struct timespec sleep_time;
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 1000000;

    struct timespec start_time;
    if (timeout >= 0.0) {
        timespec_get(&start_time, TIME_UTC);
    }

    for (;;) {
        pthread_mutex_lock(&self->lock);
        self->draining = 1;
        int empty = self->ring_size == 0 && self->pending_buffers == 0;
        pthread_mutex_unlock(&self->lock);

        if (empty) {
            Py_RETURN_TRUE;
        }

        if (timeout >= 0.0) {
            struct timespec now;
            timespec_get(&now, TIME_UTC);
            double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                ((double)(now.tv_nsec - start_time.tv_nsec) / 1000000000.0);
            if (elapsed >= timeout) {
                Py_RETURN_FALSE;
            }
        }

        Py_BEGIN_ALLOW_THREADS
        nanosleep(&sleep_time, NULL);
        Py_END_ALLOW_THREADS
    }
}

static PyObject *tachy_output_flush(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    self->draining = 0;
    pthread_mutex_unlock(&self->lock);

    Py_RETURN_NONE;
}

static PyObject *tachy_output_close(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (!self->closed && self->queue != NULL) {
        pthread_mutex_lock(&self->lock);
        self->closed = 1;
        self->started = 0;
        self->pending_buffers = 0;
        self->draining = 0;
        pthread_mutex_unlock(&self->lock);
        AudioQueueStop(self->queue, true);
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_output_write(TachyOutputStream *self, PyObject *frames)
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) != 0) {
        return NULL;
    }

    if (view.len == 0 || view.len % self->bytes_per_frame != 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "frames must contain whole interleaved float32 frames");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    size_t available = self->ring_capacity - self->ring_size;
    size_t accepted = tachy_min_size((size_t)view.len, available);
    accepted -= accepted % self->bytes_per_frame;
    if (accepted < (size_t)view.len) {
        self->overruns += 1;
    }
    if (accepted > 0) {
        self->draining = 0;
        tachy_ring_copy_in(self, (const uint8_t *)view.buf, accepted);
    }
    pthread_mutex_unlock(&self->lock);
    PyBuffer_Release(&view);

    UInt64 frames_written = accepted / self->bytes_per_frame;
    return PyLong_FromUnsignedLongLong(frames_written);
}

static PyObject *tachy_output_stats(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    pthread_mutex_lock(&self->lock);
    UInt64 frames_processed = self->frames_processed;
    UInt32 underruns = self->underruns;
    UInt32 overruns = self->overruns;
    UInt64 queued_frames = 0;
    UInt32 buffer_size = 0;
    double estimated_latency = 0.0;
    if (self->format.mSampleRate > 0 && self->bytes_per_frame > 0) {
        queued_frames = (UInt64)(self->ring_size / self->bytes_per_frame);
        buffer_size = self->buffer_byte_size / self->bytes_per_frame;
        estimated_latency = (double)queued_frames / self->format.mSampleRate;
    }
    double queued_latency = estimated_latency;
    pthread_mutex_unlock(&self->lock);

    return tachy_build_stream_stats(
        frames_processed,
        underruns,
        overruns,
        queued_frames,
        queued_latency,
        self->hardware_latency,
        1,
        buffer_size);
}

static PyMethodDef tachy_output_methods[] = {
    {"start", (PyCFunction)tachy_output_start, METH_NOARGS, "Start output playback."},
    {"stop", (PyCFunction)tachy_output_stop, METH_NOARGS, "Stop output playback."},
    {"drain", (PyCFunction)tachy_output_drain, METH_VARARGS | METH_KEYWORDS, "Wait for queued frames to drain."},
    {"flush", (PyCFunction)tachy_output_flush, METH_NOARGS, "Discard queued output frames."},
    {"close", (PyCFunction)tachy_output_close, METH_NOARGS, "Close output playback."},
    {"write", (PyCFunction)tachy_output_write, METH_O, "Write interleaved float32 frames."},
    {"stats", (PyCFunction)tachy_output_stats, METH_NOARGS, "Return stream statistics."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyOutputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.OutputStream",
    .tp_basicsize = sizeof(TachyOutputStream),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)tachy_output_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Native Core Audio output stream.",
    .tp_methods = tachy_output_methods,
    .tp_new = tachy_output_new,
};

static void tachy_input_callback(
    void *user_data,
    AudioQueueRef queue,
    AudioQueueBufferRef buffer,
    const AudioTimeStamp *start_time,
    UInt32 packet_count,
    const AudioStreamPacketDescription *packet_descriptions)
{
    (void)queue;
    (void)start_time;
    (void)packet_count;
    (void)packet_descriptions;

    TachyInputStream *stream = (TachyInputStream *)user_data;

    pthread_mutex_lock(&stream->lock);
    if (!stream->closed && buffer->mAudioDataByteSize > 0) {
        size_t incoming = buffer->mAudioDataByteSize;
        incoming -= incoming % stream->bytes_per_frame;
        size_t available = stream->ring_capacity - stream->ring_size;
        size_t accepted = tachy_min_size(incoming, available);
        accepted -= accepted % stream->bytes_per_frame;
        if (accepted < incoming) {
            stream->overruns += 1;
        }
        if (accepted > 0) {
            tachy_input_ring_copy_in(stream, (const uint8_t *)buffer->mAudioData, accepted);
            stream->frames_processed += accepted / stream->bytes_per_frame;
        }
    }
    int should_continue = stream->started && !stream->closed;
    pthread_mutex_unlock(&stream->lock);

    if (should_continue) {
        (void)AudioQueueEnqueueBuffer(stream->queue, buffer, 0, NULL);
    }
}

static PyObject *tachy_input_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "sample_rate",
        "channels",
        "block_size",
        "device_id",
        "latency",
        NULL
    };
    int sample_rate = 0;
    int channels = 0;
    int block_size = 0;
    const char *device_id = NULL;
    double latency = 0.0;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "iiizd",
            keywords,
            &sample_rate,
            &channels,
            &block_size,
            &device_id,
            &latency)) {
        return NULL;
    }

    if (sample_rate < 1 || channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate and channels must be positive");
        return NULL;
    }

    TachyInputStream *self = (TachyInputStream *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->bytes_per_frame = (UInt32)channels * sizeof(float);
    UInt32 buffer_frames = (UInt32)(sample_rate * TACHY_DEFAULT_BUFFER_MS / 1000);
    if (block_size > 0) {
        buffer_frames = (UInt32)block_size;
    }
    if (latency > 0.0) {
        UInt32 latency_frames = (UInt32)(((double)sample_rate * latency) / TACHY_INPUT_BUFFER_COUNT);
        if (latency_frames > 0) {
            buffer_frames = latency_frames;
        }
    }
    if (buffer_frames < 64) {
        buffer_frames = 64;
    }

    self->buffer_byte_size = buffer_frames * self->bytes_per_frame;
    self->frames_processed = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->hardware_latency = 0.0;
    self->ring = NULL;
    self->ring_capacity = (size_t)sample_rate * TACHY_RING_SECONDS * self->bytes_per_frame;
    if (self->ring_capacity < (size_t)self->buffer_byte_size * TACHY_INPUT_BUFFER_COUNT * 2) {
        self->ring_capacity = (size_t)self->buffer_byte_size * TACHY_INPUT_BUFFER_COUNT * 2;
    }
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    self->lock_initialized = 0;
    self->started = 0;
    self->closed = 0;
    for (UInt32 index = 0; index < TACHY_INPUT_BUFFER_COUNT; index++) {
        self->buffers[index] = NULL;
    }

    self->ring = (uint8_t *)PyMem_RawMalloc(self->ring_capacity);
    if (self->ring == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize input stream lock");
        return NULL;
    }
    self->lock_initialized = 1;

    self->format.mSampleRate = (Float64)sample_rate;
    self->format.mFormatID = kAudioFormatLinearPCM;
    self->format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    self->format.mBytesPerPacket = self->bytes_per_frame;
    self->format.mFramesPerPacket = 1;
    self->format.mBytesPerFrame = self->bytes_per_frame;
    self->format.mChannelsPerFrame = (UInt32)channels;
    self->format.mBitsPerChannel = 8 * sizeof(float);
    self->format.mReserved = 0;

    OSStatus status = AudioQueueNewInput(
        &self->format,
        tachy_input_callback,
        self,
        NULL,
        NULL,
        0,
        &self->queue);

    if (status != noErr) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to create Core Audio input queue");
        return NULL;
    }

    if (!tachy_set_input_device(self->queue, device_id)) {
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
        Py_DECREF(self);
        return NULL;
    }

    AudioObjectID input_device = device_id == NULL || device_id[0] == '\0'
        ? tachy_get_default_device(kAudioHardwarePropertyDefaultInputDevice)
        : tachy_find_device_by_uid(device_id);
    self->hardware_latency = tachy_get_coreaudio_hardware_latency(
        input_device,
        kAudioDevicePropertyScopeInput,
        (double)sample_rate);

    for (UInt32 index = 0; index < TACHY_INPUT_BUFFER_COUNT; index++) {
        status = AudioQueueAllocateBuffer(self->queue, self->buffer_byte_size, &self->buffers[index]);
        if (status != noErr || self->buffers[index] == NULL) {
            AudioQueueDispose(self->queue, true);
            self->queue = NULL;
            Py_DECREF(self);
            PyErr_SetString(PyExc_RuntimeError, "failed to allocate Core Audio input buffers");
            return NULL;
        }
    }

    return (PyObject *)self;
}

static void tachy_input_dealloc(TachyInputStream *self)
{
    if (!self->closed && self->queue != NULL) {
        pthread_mutex_lock(&self->lock);
        self->closed = 1;
        self->started = 0;
        pthread_mutex_unlock(&self->lock);
        AudioQueueStop(self->queue, true);
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
    }
    if (self->lock_initialized) {
        pthread_mutex_destroy(&self->lock);
        self->lock_initialized = 0;
    }
    if (self->ring != NULL) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_input_start(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    if (self->started) {
        Py_RETURN_NONE;
    }

    pthread_mutex_lock(&self->lock);
    self->started = 1;
    pthread_mutex_unlock(&self->lock);

    for (UInt32 index = 0; index < TACHY_INPUT_BUFFER_COUNT; index++) {
        OSStatus enqueue_status = AudioQueueEnqueueBuffer(self->queue, self->buffers[index], 0, NULL);
        if (enqueue_status != noErr) {
            pthread_mutex_lock(&self->lock);
            self->started = 0;
            pthread_mutex_unlock(&self->lock);
            PyErr_SetString(PyExc_RuntimeError, "failed to enqueue Core Audio input buffer");
            return NULL;
        }
    }

    OSStatus status = AudioQueueStart(self->queue, NULL);
    if (status != noErr) {
        pthread_mutex_lock(&self->lock);
        self->started = 0;
        pthread_mutex_unlock(&self->lock);
        PyErr_SetString(PyExc_RuntimeError, "failed to start Core Audio input queue");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_input_stop(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->started = 0;
    pthread_mutex_unlock(&self->lock);

    OSStatus status = AudioQueueStop(self->queue, false);
    if (status != noErr) {
        PyErr_SetString(PyExc_RuntimeError, "failed to stop Core Audio input queue");
        return NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_input_close(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (!self->closed && self->queue != NULL) {
        pthread_mutex_lock(&self->lock);
        self->closed = 1;
        self->started = 0;
        pthread_mutex_unlock(&self->lock);
        AudioQueueStop(self->queue, true);
        AudioQueueDispose(self->queue, true);
        self->queue = NULL;
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_input_read(TachyInputStream *self, PyObject *args)
{
    int frame_count = 0;
    if (!PyArg_ParseTuple(args, "i", &frame_count)) {
        return NULL;
    }
    if (frame_count < 1) {
        PyErr_SetString(PyExc_ValueError, "frame_count must be positive");
        return NULL;
    }
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    size_t requested = (size_t)frame_count * self->bytes_per_frame;

    pthread_mutex_lock(&self->lock);
    size_t copied = tachy_min_size(requested, self->ring_size);
    copied -= copied % self->bytes_per_frame;
    if (copied < requested) {
        self->underruns += 1;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)copied);
    if (result != NULL && copied > 0) {
        char *target = PyBytes_AS_STRING(result);
        (void)tachy_input_ring_copy_out(self, (uint8_t *)target, copied);
    }
    pthread_mutex_unlock(&self->lock);

    return result;
}

static PyObject *tachy_input_flush(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->queue == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    pthread_mutex_unlock(&self->lock);

    Py_RETURN_NONE;
}

static PyObject *tachy_input_stats(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    pthread_mutex_lock(&self->lock);
    UInt64 frames_processed = self->frames_processed;
    UInt32 underruns = self->underruns;
    UInt32 overruns = self->overruns;
    UInt64 queued_frames = 0;
    UInt32 buffer_size = 0;
    double estimated_latency = 0.0;
    if (self->format.mSampleRate > 0 && self->bytes_per_frame > 0) {
        queued_frames = (UInt64)(self->ring_size / self->bytes_per_frame);
        buffer_size = self->buffer_byte_size / self->bytes_per_frame;
        estimated_latency = (double)queued_frames / self->format.mSampleRate;
    }
    double queued_latency = estimated_latency;
    pthread_mutex_unlock(&self->lock);

    return tachy_build_stream_stats(
        frames_processed,
        underruns,
        overruns,
        queued_frames,
        queued_latency,
        self->hardware_latency,
        1,
        buffer_size);
}

static PyMethodDef tachy_input_methods[] = {
    {"start", (PyCFunction)tachy_input_start, METH_NOARGS, "Start input capture."},
    {"stop", (PyCFunction)tachy_input_stop, METH_NOARGS, "Stop input capture."},
    {"close", (PyCFunction)tachy_input_close, METH_NOARGS, "Close input capture."},
    {"read", (PyCFunction)tachy_input_read, METH_VARARGS, "Read available interleaved float32 frames."},
    {"flush", (PyCFunction)tachy_input_flush, METH_NOARGS, "Discard captured frames."},
    {"stats", (PyCFunction)tachy_input_stats, METH_NOARGS, "Return stream statistics."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyInputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.InputStream",
    .tp_basicsize = sizeof(TachyInputStream),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)tachy_input_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Native Core Audio input stream.",
    .tp_methods = tachy_input_methods,
    .tp_new = tachy_input_new,
};

typedef struct {
    PyObject_HEAD
    TachyInputStream *input;
    TachyOutputStream *output;
    int closed;
} TachyDuplexStream;

static PyObject *tachy_duplex_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "sample_rate",
        "input_channels",
        "output_channels",
        "block_size",
        "input_device_id",
        "output_device_id",
        "latency",
        NULL
    };
    int sample_rate = 0;
    int input_channels = 0;
    int output_channels = 0;
    int block_size = 0;
    const char *input_device_id = NULL;
    const char *output_device_id = NULL;
    double latency = 0.0;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "iiiizzd",
            keywords,
            &sample_rate,
            &input_channels,
            &output_channels,
            &block_size,
            &input_device_id,
            &output_device_id,
            &latency)) {
        return NULL;
    }

    if (sample_rate < 1 || input_channels < 1 || output_channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate and channel counts must be positive");
        return NULL;
    }

    TachyDuplexStream *self = (TachyDuplexStream *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->input = NULL;
    self->output = NULL;
    self->closed = 0;

    PyObject *input_args = Py_BuildValue(
        "(iiizd)",
        sample_rate,
        input_channels,
        block_size,
        input_device_id,
        latency);
    if (input_args == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->input = (TachyInputStream *)PyObject_CallObject((PyObject *)&TachyInputStreamType, input_args);
    Py_DECREF(input_args);
    if (self->input == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    PyObject *output_args = Py_BuildValue(
        "(iiizd)",
        sample_rate,
        output_channels,
        block_size,
        output_device_id,
        latency);
    if (output_args == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    self->output = (TachyOutputStream *)PyObject_CallObject((PyObject *)&TachyOutputStreamType, output_args);
    Py_DECREF(output_args);
    if (self->output == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    return (PyObject *)self;
}

static void tachy_duplex_dealloc(TachyDuplexStream *self)
{
    if (!self->closed) {
        self->closed = 1;
        if (self->output != NULL) {
            (void)tachy_output_close(self->output, NULL);
        }
        if (self->input != NULL) {
            (void)tachy_input_close(self->input, NULL);
        }
    }
    Py_CLEAR(self->output);
    Py_CLEAR(self->input);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_duplex_start(TachyDuplexStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->input == NULL || self->output == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }

    PyObject *input_result = tachy_input_start(self->input, NULL);
    if (input_result == NULL) {
        return NULL;
    }
    Py_DECREF(input_result);

    PyObject *output_result = tachy_output_start(self->output, NULL);
    if (output_result == NULL) {
        (void)tachy_input_stop(self->input, NULL);
        return NULL;
    }
    Py_DECREF(output_result);
    Py_RETURN_NONE;
}

static PyObject *tachy_duplex_stop(TachyDuplexStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->input == NULL || self->output == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }

    PyObject *output_result = tachy_output_stop(self->output, NULL);
    if (output_result == NULL) {
        return NULL;
    }
    Py_DECREF(output_result);

    PyObject *input_result = tachy_input_stop(self->input, NULL);
    if (input_result == NULL) {
        return NULL;
    }
    Py_DECREF(input_result);
    Py_RETURN_NONE;
}

static PyObject *tachy_duplex_flush(TachyDuplexStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->input == NULL || self->output == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }

    PyObject *output_result = tachy_output_flush(self->output, NULL);
    if (output_result == NULL) {
        return NULL;
    }
    Py_DECREF(output_result);

    PyObject *input_result = tachy_input_flush(self->input, NULL);
    if (input_result == NULL) {
        return NULL;
    }
    Py_DECREF(input_result);
    Py_RETURN_NONE;
}

static PyObject *tachy_duplex_close(TachyDuplexStream *self, PyObject *Py_UNUSED(ignored))
{
    if (!self->closed) {
        self->closed = 1;
        if (self->output != NULL) {
            PyObject *output_result = tachy_output_close(self->output, NULL);
            Py_XDECREF(output_result);
        }
        if (self->input != NULL) {
            PyObject *input_result = tachy_input_close(self->input, NULL);
            Py_XDECREF(input_result);
        }
    }
    Py_RETURN_NONE;
}

static PyObject *tachy_duplex_write(TachyDuplexStream *self, PyObject *frames)
{
    if (self->closed || self->output == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }
    return tachy_output_write(self->output, frames);
}

static PyObject *tachy_duplex_read(TachyDuplexStream *self, PyObject *args)
{
    if (self->closed || self->input == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }
    return tachy_input_read(self->input, args);
}

static PyObject *tachy_duplex_stats(TachyDuplexStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || self->input == NULL || self->output == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "duplex stream is closed");
        return NULL;
    }

    PyObject *input_stats = tachy_input_stats(self->input, NULL);
    if (input_stats == NULL) {
        return NULL;
    }
    PyObject *output_stats = tachy_output_stats(self->output, NULL);
    if (output_stats == NULL) {
        Py_DECREF(input_stats);
        return NULL;
    }
    PyObject *stats = Py_BuildValue("{s:O,s:O}", "input", input_stats, "output", output_stats);
    Py_DECREF(input_stats);
    Py_DECREF(output_stats);
    return stats;
}

static PyMethodDef tachy_duplex_methods[] = {
    {"start", (PyCFunction)tachy_duplex_start, METH_NOARGS, "Start duplex capture and playback."},
    {"stop", (PyCFunction)tachy_duplex_stop, METH_NOARGS, "Stop duplex capture and playback."},
    {"flush", (PyCFunction)tachy_duplex_flush, METH_NOARGS, "Discard queued duplex frames."},
    {"close", (PyCFunction)tachy_duplex_close, METH_NOARGS, "Close duplex stream."},
    {"write", (PyCFunction)tachy_duplex_write, METH_O, "Write interleaved output float32 frames."},
    {"read", (PyCFunction)tachy_duplex_read, METH_VARARGS, "Read available interleaved input float32 frames."},
    {"stats", (PyCFunction)tachy_duplex_stats, METH_NOARGS, "Return duplex stream statistics."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyDuplexStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.DuplexStream",
    .tp_basicsize = sizeof(TachyDuplexStream),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)tachy_duplex_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Native Core Audio duplex stream.",
    .tp_methods = tachy_duplex_methods,
    .tp_new = tachy_duplex_new,
};

static int tachy_get_cf_string(AudioObjectID object_id, AudioObjectPropertySelector selector, char *buffer, CFIndex buffer_size)
{
    AudioObjectPropertyAddress address = {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    CFStringRef value = NULL;
    UInt32 size = sizeof(value);
    OSStatus status = AudioObjectGetPropertyData(object_id, &address, 0, NULL, &size, &value);

    if (status != noErr || value == NULL) {
        return 0;
    }

    Boolean ok = CFStringGetCString(value, buffer, buffer_size, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok ? 1 : 0;
}

static UInt32 tachy_get_channel_count(AudioObjectID device_id, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(device_id, &address, 0, NULL, &size);

    if (status != noErr || size == 0) {
        return 0;
    }

    AudioBufferList *buffer_list = (AudioBufferList *)PyMem_RawMalloc(size);
    if (buffer_list == NULL) {
        return 0;
    }

    status = AudioObjectGetPropertyData(device_id, &address, 0, NULL, &size, buffer_list);
    if (status != noErr) {
        PyMem_RawFree(buffer_list);
        return 0;
    }

    UInt32 channels = 0;
    for (UInt32 index = 0; index < buffer_list->mNumberBuffers; index++) {
        channels += buffer_list->mBuffers[index].mNumberChannels;
    }

    PyMem_RawFree(buffer_list);
    return channels;
}

static double tachy_get_sample_rate(AudioObjectID device_id)
{
    AudioObjectPropertyAddress address = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    Float64 sample_rate = 0;
    UInt32 size = sizeof(sample_rate);
    OSStatus status = AudioObjectGetPropertyData(device_id, &address, 0, NULL, &size, &sample_rate);

    if (status != noErr) {
        return 0;
    }

    return sample_rate;
}

static AudioObjectID tachy_get_default_device(AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress address = {
        selector,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectID device_id = kAudioObjectUnknown;
    UInt32 size = sizeof(device_id);
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device_id);

    if (status != noErr) {
        return kAudioObjectUnknown;
    }

    return device_id;
}

static AudioObjectID tachy_find_device_by_uid(const char *device_uid)
{
    if (device_uid == NULL || device_uid[0] == '\0') {
        return kAudioObjectUnknown;
    }

    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size);

    if (status != noErr || size == 0) {
        return kAudioObjectUnknown;
    }

    AudioObjectID *device_ids = (AudioObjectID *)PyMem_RawMalloc(size);
    if (device_ids == NULL) {
        return kAudioObjectUnknown;
    }

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, device_ids);
    if (status != noErr) {
        PyMem_RawFree(device_ids);
        return kAudioObjectUnknown;
    }

    AudioObjectID found_device = kAudioObjectUnknown;
    UInt32 device_count = size / sizeof(AudioObjectID);
    for (UInt32 index = 0; index < device_count; index++) {
        char uid[256] = {0};
        if (tachy_get_cf_string(device_ids[index], kAudioDevicePropertyDeviceUID, uid, sizeof(uid)) &&
                strcmp(uid, device_uid) == 0) {
            found_device = device_ids[index];
            break;
        }
    }

    PyMem_RawFree(device_ids);
    return found_device;
}

static UInt32 tachy_get_coreaudio_uint32_property(
    AudioObjectID device_id,
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope)
{
    if (device_id == kAudioObjectUnknown) {
        return 0;
    }

    AudioObjectPropertyAddress address = {
        selector,
        scope,
        kAudioObjectPropertyElementMain
    };
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    OSStatus status = AudioObjectGetPropertyData(device_id, &address, 0, NULL, &size, &value);

    if (status != noErr) {
        return 0;
    }

    return value;
}

static double tachy_get_coreaudio_hardware_latency(
    AudioObjectID device_id,
    AudioObjectPropertyScope scope,
    double sample_rate)
{
    if (sample_rate <= 0.0) {
        return 0.0;
    }

    UInt32 latency_frames = tachy_get_coreaudio_uint32_property(
        device_id,
        kAudioDevicePropertyLatency,
        scope);
    UInt32 safety_offset_frames = tachy_get_coreaudio_uint32_property(
        device_id,
        kAudioDevicePropertySafetyOffset,
        scope);

    return (double)(latency_frames + safety_offset_frames) / sample_rate;
}

static int tachy_append_device(PyObject *devices, AudioObjectID device_id, const char *kind, UInt32 channels, int is_default)
{
    char uid[256] = {0};
    char name[256] = {0};
    double sample_rate = tachy_get_sample_rate(device_id);
    PyObject *entry = NULL;
    int ok = 0;

    if (!tachy_get_cf_string(device_id, kAudioDevicePropertyDeviceUID, uid, sizeof(uid))) {
        snprintf(uid, sizeof(uid), "%u", (unsigned int)device_id);
    }

    if (!tachy_get_cf_string(device_id, kAudioObjectPropertyName, name, sizeof(name))) {
        snprintf(name, sizeof(name), "Audio Device %u", (unsigned int)device_id);
    }

    entry = Py_BuildValue(
        "{s:s,s:s,s:s,s:I,s:d,s:O}",
        "id", uid,
        "name", name,
        "kind", kind,
        "channels", channels,
        "default_sample_rate", sample_rate,
        "is_default", is_default ? Py_True : Py_False
    );

    if (entry == NULL) {
        return 0;
    }

    ok = PyList_Append(devices, entry) == 0;
    Py_DECREF(entry);
    return ok;
}

static PyObject *tachy_list_devices(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;

    AudioObjectPropertyAddress address = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size);

    if (status != noErr) {
        PyErr_SetString(PyExc_RuntimeError, "Core Audio device enumeration failed");
        return NULL;
    }

    AudioObjectID *device_ids = (AudioObjectID *)PyMem_RawMalloc(size);
    if (device_ids == NULL) {
        return PyErr_NoMemory();
    }

    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, device_ids);
    if (status != noErr) {
        PyMem_RawFree(device_ids);
        PyErr_SetString(PyExc_RuntimeError, "Core Audio device enumeration failed");
        return NULL;
    }

    AudioObjectID default_output = tachy_get_default_device(kAudioHardwarePropertyDefaultOutputDevice);
    AudioObjectID default_input = tachy_get_default_device(kAudioHardwarePropertyDefaultInputDevice);
    UInt32 device_count = size / sizeof(AudioObjectID);
    PyObject *devices = PyList_New(0);

    if (devices == NULL) {
        PyMem_RawFree(device_ids);
        return NULL;
    }

    for (UInt32 index = 0; index < device_count; index++) {
        AudioObjectID device_id = device_ids[index];
        UInt32 output_channels = tachy_get_channel_count(device_id, kAudioDevicePropertyScopeOutput);
        UInt32 input_channels = tachy_get_channel_count(device_id, kAudioDevicePropertyScopeInput);

        if (output_channels > 0 && input_channels > 0) {
            if (!tachy_append_device(
                    devices,
                    device_id,
                    "duplex",
                    output_channels > input_channels ? output_channels : input_channels,
                    device_id == default_output || device_id == default_input)) {
                Py_DECREF(devices);
                PyMem_RawFree(device_ids);
                return NULL;
            }
        } else if (output_channels > 0) {
            if (!tachy_append_device(devices, device_id, "output", output_channels, device_id == default_output)) {
                Py_DECREF(devices);
                PyMem_RawFree(device_ids);
                return NULL;
            }
        } else if (input_channels > 0) {
            if (!tachy_append_device(devices, device_id, "input", input_channels, device_id == default_input)) {
                Py_DECREF(devices);
                PyMem_RawFree(device_ids);
                return NULL;
            }
        }
    }

    PyMem_RawFree(device_ids);
    return devices;
}

#elif defined(__linux__)

typedef struct {
    PyObject_HEAD
    ma_context context;
    ma_device device;
    ma_device_id device_id;
    ma_uint32 sample_rate;
    ma_uint32 channels;
    ma_uint32 buffer_frames;
    ma_uint32 bytes_per_frame;
    ma_uint64 frames_processed;
    ma_uint32 underruns;
    ma_uint32 overruns;
    uint8_t *ring;
    size_t ring_capacity;
    size_t ring_read;
    size_t ring_write;
    size_t ring_size;
    pthread_mutex_t lock;
    int lock_initialized;
    int context_initialized;
    int device_initialized;
    int started;
    int closed;
} TachyOutputStream;

typedef struct {
    PyObject_HEAD
    ma_context context;
    ma_device device;
    ma_device_id device_id;
    ma_uint32 sample_rate;
    ma_uint32 channels;
    ma_uint32 buffer_frames;
    ma_uint32 bytes_per_frame;
    ma_uint64 frames_processed;
    ma_uint32 underruns;
    ma_uint32 overruns;
    uint8_t *ring;
    size_t ring_capacity;
    size_t ring_read;
    size_t ring_write;
    size_t ring_size;
    pthread_mutex_t lock;
    int lock_initialized;
    int context_initialized;
    int device_initialized;
    int started;
    int closed;
} TachyInputStream;

static ma_result tachy_miniaudio_context_init(ma_context *context)
{
    ma_backend backends[] = {
        ma_backend_pulseaudio,
        ma_backend_alsa,
    };

    return ma_context_init(backends, sizeof(backends) / sizeof(backends[0]), NULL, context);
}

static void tachy_miniaudio_ring_copy_in(TachyOutputStream *stream, const uint8_t *source, size_t byte_count)
{
    tachy_ring_copy_in_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_write,
        &stream->ring_size,
        source,
        byte_count);
}

static size_t tachy_miniaudio_ring_copy_out(TachyOutputStream *stream, uint8_t *target, size_t byte_count)
{
    return tachy_ring_copy_out_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_read,
        &stream->ring_size,
        target,
        byte_count);
}

static void tachy_miniaudio_output_callback(
    ma_device *device,
    void *output,
    const void *input,
    ma_uint32 frame_count)
{
    (void)input;
    TachyOutputStream *stream = (TachyOutputStream *)device->pUserData;
    size_t byte_count = (size_t)frame_count * stream->bytes_per_frame;

    pthread_mutex_lock(&stream->lock);
    size_t copied = tachy_miniaudio_ring_copy_out(stream, (uint8_t *)output, byte_count);
    if (copied < byte_count) {
        memset((uint8_t *)output + copied, 0, byte_count - copied);
        stream->underruns += 1;
    }
    stream->frames_processed += frame_count;
    pthread_mutex_unlock(&stream->lock);
}

static int tachy_miniaudio_set_device_id(
    TachyOutputStream *stream,
    ma_device_config *config,
    const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return 1;
    }

    const char *prefix = "output-";
    size_t prefix_length = strlen(prefix);
    if (strncmp(device_id, prefix, prefix_length) != 0) {
        PyErr_SetString(PyExc_ValueError, "Linux output device_id must come from an output device");
        return 0;
    }

    const char *backend_id = device_id + prefix_length;
    memset(&stream->device_id, 0, sizeof(stream->device_id));

    switch (stream->context.backend) {
        case ma_backend_alsa:
            snprintf(stream->device_id.alsa, sizeof(stream->device_id.alsa), "%s", backend_id);
            break;
        case ma_backend_pulseaudio:
            snprintf(stream->device_id.pulse, sizeof(stream->device_id.pulse), "%s", backend_id);
            break;
        case ma_backend_jack:
            stream->device_id.jack = 0;
            break;
        case ma_backend_sndio:
            snprintf(stream->device_id.sndio, sizeof(stream->device_id.sndio), "%s", backend_id);
            break;
        case ma_backend_audio4:
            snprintf(stream->device_id.audio4, sizeof(stream->device_id.audio4), "%s", backend_id);
            break;
        case ma_backend_oss:
            snprintf(stream->device_id.oss, sizeof(stream->device_id.oss), "%s", backend_id);
            break;
        default:
            PyErr_SetString(PyExc_ValueError, "selected Linux audio backend does not support explicit device_id yet");
            return 0;
    }

    config->playback.pDeviceID = &stream->device_id;
    return 1;
}

static PyObject *tachy_output_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "sample_rate",
        "channels",
        "block_size",
        "device_id",
        "latency",
        NULL
    };
    int sample_rate = 0;
    int channels = 0;
    int block_size = 0;
    const char *device_id = NULL;
    double latency = 0.0;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "iiizd",
            keywords,
            &sample_rate,
            &channels,
            &block_size,
            &device_id,
            &latency)) {
        return NULL;
    }

    if (sample_rate < 1 || channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate and channels must be positive");
        return NULL;
    }

    TachyOutputStream *self = (TachyOutputStream *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->sample_rate = (ma_uint32)sample_rate;
    self->channels = (ma_uint32)channels;
    self->bytes_per_frame = (ma_uint32)channels * sizeof(float);
    self->buffer_frames = (ma_uint32)(sample_rate * TACHY_DEFAULT_BUFFER_MS / 1000);
    if (block_size > 0) {
        self->buffer_frames = (ma_uint32)block_size;
    }
    if (latency > 0.0) {
        ma_uint32 latency_frames = (ma_uint32)(((double)sample_rate * latency) / TACHY_OUTPUT_BUFFER_COUNT);
        if (latency_frames > 0) {
            self->buffer_frames = latency_frames;
        }
    }
    if (self->buffer_frames < 64) {
        self->buffer_frames = 64;
    }
    self->frames_processed = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->ring = NULL;
    self->ring_capacity = (size_t)sample_rate * TACHY_RING_SECONDS * self->bytes_per_frame;
    if (self->ring_capacity < (size_t)self->buffer_frames * self->bytes_per_frame * TACHY_OUTPUT_BUFFER_COUNT * 2) {
        self->ring_capacity = (size_t)self->buffer_frames * self->bytes_per_frame * TACHY_OUTPUT_BUFFER_COUNT * 2;
    }
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    self->lock_initialized = 0;
    self->context_initialized = 0;
    self->device_initialized = 0;
    self->started = 0;
    self->closed = 0;

    self->ring = (uint8_t *)PyMem_RawMalloc(self->ring_capacity);
    if (self->ring == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize output stream lock");
        return NULL;
    }
    self->lock_initialized = 1;

    ma_result result = tachy_miniaudio_context_init(&self->context);
    if (result != MA_SUCCESS) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize miniaudio context");
        return NULL;
    }
    self->context_initialized = 1;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.sampleRate = self->sample_rate;
    config.periodSizeInFrames = self->buffer_frames;
    config.periods = TACHY_OUTPUT_BUFFER_COUNT;
    config.playback.format = ma_format_f32;
    config.playback.channels = self->channels;
    config.dataCallback = tachy_miniaudio_output_callback;
    config.pUserData = self;

    if (!tachy_miniaudio_set_device_id(self, &config, device_id)) {
        Py_DECREF(self);
        return NULL;
    }

    result = ma_device_init(&self->context, &config, &self->device);
    if (result != MA_SUCCESS) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize miniaudio output device");
        return NULL;
    }
    self->device_initialized = 1;

    return (PyObject *)self;
}

static void tachy_output_dealloc(TachyOutputStream *self)
{
    self->closed = 1;
    self->started = 0;
    if (self->device_initialized) {
        ma_device_uninit(&self->device);
        self->device_initialized = 0;
    }
    if (self->context_initialized) {
        ma_context_uninit(&self->context);
        self->context_initialized = 0;
    }
    if (self->lock_initialized) {
        pthread_mutex_destroy(&self->lock);
        self->lock_initialized = 0;
    }
    if (self->ring != NULL) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_output_start(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    if (self->started) {
        Py_RETURN_NONE;
    }

    ma_result result = ma_device_start(&self->device);
    if (result != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to start miniaudio output device");
        return NULL;
    }

    self->started = 1;
    Py_RETURN_NONE;
}

static PyObject *tachy_output_stop(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    ma_result result = ma_device_stop(&self->device);
    if (result != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to stop miniaudio output device");
        return NULL;
    }

    self->started = 0;
    Py_RETURN_NONE;
}

static PyObject *tachy_output_drain(TachyOutputStream *self, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"timeout", NULL};
    double timeout = -1.0;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|d", keywords, &timeout)) {
        return NULL;
    }

    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    struct timespec sleep_time;
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 1000000;

    struct timespec start_time;
    if (timeout >= 0.0) {
        timespec_get(&start_time, TIME_UTC);
    }

    for (;;) {
        pthread_mutex_lock(&self->lock);
        int empty = self->ring_size == 0;
        pthread_mutex_unlock(&self->lock);

        if (empty) {
            Py_RETURN_TRUE;
        }

        if (timeout >= 0.0) {
            struct timespec now;
            timespec_get(&now, TIME_UTC);
            double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                ((double)(now.tv_nsec - start_time.tv_nsec) / 1000000000.0);
            if (elapsed >= timeout) {
                Py_RETURN_FALSE;
            }
        }

        Py_BEGIN_ALLOW_THREADS
        nanosleep(&sleep_time, NULL);
        Py_END_ALLOW_THREADS
    }
}

static PyObject *tachy_output_flush(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    pthread_mutex_unlock(&self->lock);

    Py_RETURN_NONE;
}

static PyObject *tachy_output_close(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (!self->closed) {
        self->closed = 1;
        self->started = 0;
        if (self->device_initialized) {
            ma_device_uninit(&self->device);
            self->device_initialized = 0;
        }
        if (self->context_initialized) {
            ma_context_uninit(&self->context);
            self->context_initialized = 0;
        }
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_output_write(TachyOutputStream *self, PyObject *frames)
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "output stream is closed");
        return NULL;
    }

    Py_buffer view;
    if (PyObject_GetBuffer(frames, &view, PyBUF_CONTIG_RO) != 0) {
        return NULL;
    }

    if (view.len == 0 || view.len % self->bytes_per_frame != 0) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_ValueError, "frames must contain whole interleaved float32 frames");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    size_t available = self->ring_capacity - self->ring_size;
    size_t accepted = tachy_min_size((size_t)view.len, available);
    accepted -= accepted % self->bytes_per_frame;
    if (accepted < (size_t)view.len) {
        self->overruns += 1;
    }
    if (accepted > 0) {
        tachy_miniaudio_ring_copy_in(self, (const uint8_t *)view.buf, accepted);
    }
    pthread_mutex_unlock(&self->lock);
    PyBuffer_Release(&view);

    ma_uint64 frames_written = accepted / self->bytes_per_frame;
    return PyLong_FromUnsignedLongLong(frames_written);
}

static PyObject *tachy_output_stats(TachyOutputStream *self, PyObject *Py_UNUSED(ignored))
{
    pthread_mutex_lock(&self->lock);
    ma_uint64 frames_processed = self->frames_processed;
    ma_uint32 underruns = self->underruns;
    ma_uint32 overruns = self->overruns;
    ma_uint64 queued_frames = 0;
    double estimated_latency = 0.0;
    if (self->sample_rate > 0 && self->bytes_per_frame > 0) {
        queued_frames = (ma_uint64)(self->ring_size / self->bytes_per_frame);
        estimated_latency = (double)queued_frames / self->sample_rate;
    }
    double queued_latency = estimated_latency;
    ma_uint32 buffer_size = self->buffer_frames;
    pthread_mutex_unlock(&self->lock);

    return tachy_build_stream_stats_without_hardware_latency(
        frames_processed,
        underruns,
        overruns,
        queued_frames,
        queued_latency,
        buffer_size);
}

static PyMethodDef tachy_output_methods[] = {
    {"start", (PyCFunction)tachy_output_start, METH_NOARGS, "Start output playback."},
    {"stop", (PyCFunction)tachy_output_stop, METH_NOARGS, "Stop output playback."},
    {"drain", (PyCFunction)tachy_output_drain, METH_VARARGS | METH_KEYWORDS, "Wait for queued frames to drain."},
    {"flush", (PyCFunction)tachy_output_flush, METH_NOARGS, "Discard queued output frames."},
    {"close", (PyCFunction)tachy_output_close, METH_NOARGS, "Close output playback."},
    {"write", (PyCFunction)tachy_output_write, METH_O, "Write interleaved float32 frames."},
    {"stats", (PyCFunction)tachy_output_stats, METH_NOARGS, "Return stream statistics."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyOutputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.OutputStream",
    .tp_basicsize = sizeof(TachyOutputStream),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)tachy_output_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Native miniaudio output stream.",
    .tp_methods = tachy_output_methods,
    .tp_new = tachy_output_new,
};

static void tachy_miniaudio_input_ring_copy_in(
    TachyInputStream *stream,
    const uint8_t *source,
    size_t byte_count)
{
    tachy_ring_copy_in_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_write,
        &stream->ring_size,
        source,
        byte_count);
}

static size_t tachy_miniaudio_input_ring_copy_out(
    TachyInputStream *stream,
    uint8_t *target,
    size_t byte_count)
{
    return tachy_ring_copy_out_raw(
        stream->ring,
        stream->ring_capacity,
        &stream->ring_read,
        &stream->ring_size,
        target,
        byte_count);
}

static void tachy_miniaudio_input_callback(
    ma_device *device,
    void *output,
    const void *input,
    ma_uint32 frame_count)
{
    (void)output;
    TachyInputStream *stream = (TachyInputStream *)device->pUserData;
    if (input == NULL) {
        return;
    }

    size_t byte_count = (size_t)frame_count * stream->bytes_per_frame;

    pthread_mutex_lock(&stream->lock);
    size_t available = stream->ring_capacity - stream->ring_size;
    size_t accepted = tachy_min_size(byte_count, available);
    accepted -= accepted % stream->bytes_per_frame;
    if (accepted < byte_count) {
        stream->overruns += 1;
    }
    if (accepted > 0) {
        tachy_miniaudio_input_ring_copy_in(stream, (const uint8_t *)input, accepted);
        stream->frames_processed += accepted / stream->bytes_per_frame;
    }
    pthread_mutex_unlock(&stream->lock);
}

static int tachy_miniaudio_set_input_device_id(
    TachyInputStream *stream,
    ma_device_config *config,
    const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return 1;
    }

    const char *prefix = "input-";
    size_t prefix_length = strlen(prefix);
    if (strncmp(device_id, prefix, prefix_length) != 0) {
        PyErr_SetString(PyExc_ValueError, "Linux input device_id must come from an input device");
        return 0;
    }

    const char *backend_id = device_id + prefix_length;
    memset(&stream->device_id, 0, sizeof(stream->device_id));

    switch (stream->context.backend) {
        case ma_backend_alsa:
            snprintf(stream->device_id.alsa, sizeof(stream->device_id.alsa), "%s", backend_id);
            break;
        case ma_backend_pulseaudio:
            snprintf(stream->device_id.pulse, sizeof(stream->device_id.pulse), "%s", backend_id);
            break;
        case ma_backend_jack:
            stream->device_id.jack = 0;
            break;
        case ma_backend_sndio:
            snprintf(stream->device_id.sndio, sizeof(stream->device_id.sndio), "%s", backend_id);
            break;
        case ma_backend_audio4:
            snprintf(stream->device_id.audio4, sizeof(stream->device_id.audio4), "%s", backend_id);
            break;
        case ma_backend_oss:
            snprintf(stream->device_id.oss, sizeof(stream->device_id.oss), "%s", backend_id);
            break;
        default:
            PyErr_SetString(PyExc_ValueError, "selected Linux audio backend does not support explicit device_id yet");
            return 0;
    }

    config->capture.pDeviceID = &stream->device_id;
    return 1;
}

static PyObject *tachy_input_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {
        "sample_rate",
        "channels",
        "block_size",
        "device_id",
        "latency",
        NULL
    };
    int sample_rate = 0;
    int channels = 0;
    int block_size = 0;
    const char *device_id = NULL;
    double latency = 0.0;

    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "iiizd",
            keywords,
            &sample_rate,
            &channels,
            &block_size,
            &device_id,
            &latency)) {
        return NULL;
    }

    if (sample_rate < 1 || channels < 1) {
        PyErr_SetString(PyExc_ValueError, "sample_rate and channels must be positive");
        return NULL;
    }

    TachyInputStream *self = (TachyInputStream *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->sample_rate = (ma_uint32)sample_rate;
    self->channels = (ma_uint32)channels;
    self->bytes_per_frame = (ma_uint32)channels * sizeof(float);
    self->buffer_frames = (ma_uint32)(sample_rate * TACHY_DEFAULT_BUFFER_MS / 1000);
    if (block_size > 0) {
        self->buffer_frames = (ma_uint32)block_size;
    }
    if (latency > 0.0) {
        ma_uint32 latency_frames = (ma_uint32)(((double)sample_rate * latency) / TACHY_INPUT_BUFFER_COUNT);
        if (latency_frames > 0) {
            self->buffer_frames = latency_frames;
        }
    }
    if (self->buffer_frames < 64) {
        self->buffer_frames = 64;
    }
    self->frames_processed = 0;
    self->underruns = 0;
    self->overruns = 0;
    self->ring = NULL;
    self->ring_capacity = (size_t)sample_rate * TACHY_RING_SECONDS * self->bytes_per_frame;
    if (self->ring_capacity < (size_t)self->buffer_frames * self->bytes_per_frame * TACHY_INPUT_BUFFER_COUNT * 2) {
        self->ring_capacity = (size_t)self->buffer_frames * self->bytes_per_frame * TACHY_INPUT_BUFFER_COUNT * 2;
    }
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    self->lock_initialized = 0;
    self->context_initialized = 0;
    self->device_initialized = 0;
    self->started = 0;
    self->closed = 0;

    self->ring = (uint8_t *)PyMem_RawMalloc(self->ring_capacity);
    if (self->ring == NULL) {
        Py_DECREF(self);
        return PyErr_NoMemory();
    }

    if (pthread_mutex_init(&self->lock, NULL) != 0) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize input stream lock");
        return NULL;
    }
    self->lock_initialized = 1;

    ma_result result = tachy_miniaudio_context_init(&self->context);
    if (result != MA_SUCCESS) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize miniaudio context");
        return NULL;
    }
    self->context_initialized = 1;

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.sampleRate = self->sample_rate;
    config.periodSizeInFrames = self->buffer_frames;
    config.periods = TACHY_INPUT_BUFFER_COUNT;
    config.capture.format = ma_format_f32;
    config.capture.channels = self->channels;
    config.dataCallback = tachy_miniaudio_input_callback;
    config.pUserData = self;

    if (!tachy_miniaudio_set_input_device_id(self, &config, device_id)) {
        Py_DECREF(self);
        return NULL;
    }

    result = ma_device_init(&self->context, &config, &self->device);
    if (result != MA_SUCCESS) {
        Py_DECREF(self);
        PyErr_SetString(PyExc_RuntimeError, "failed to initialize miniaudio input device");
        return NULL;
    }
    self->device_initialized = 1;

    return (PyObject *)self;
}

static void tachy_input_dealloc(TachyInputStream *self)
{
    self->closed = 1;
    self->started = 0;
    if (self->device_initialized) {
        ma_device_uninit(&self->device);
        self->device_initialized = 0;
    }
    if (self->context_initialized) {
        ma_context_uninit(&self->context);
        self->context_initialized = 0;
    }
    if (self->lock_initialized) {
        pthread_mutex_destroy(&self->lock);
        self->lock_initialized = 0;
    }
    if (self->ring != NULL) {
        PyMem_RawFree(self->ring);
        self->ring = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_input_start(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    if (self->started) {
        Py_RETURN_NONE;
    }

    ma_result result = ma_device_start(&self->device);
    if (result != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to start miniaudio input device");
        return NULL;
    }

    self->started = 1;
    Py_RETURN_NONE;
}

static PyObject *tachy_input_stop(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    ma_result result = ma_device_stop(&self->device);
    if (result != MA_SUCCESS) {
        PyErr_SetString(PyExc_RuntimeError, "failed to stop miniaudio input device");
        return NULL;
    }

    self->started = 0;
    Py_RETURN_NONE;
}

static PyObject *tachy_input_close(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (!self->closed) {
        self->closed = 1;
        self->started = 0;
        if (self->device_initialized) {
            ma_device_uninit(&self->device);
            self->device_initialized = 0;
        }
        if (self->context_initialized) {
            ma_context_uninit(&self->context);
            self->context_initialized = 0;
        }
    }

    Py_RETURN_NONE;
}

static PyObject *tachy_input_read(TachyInputStream *self, PyObject *args)
{
    int frame_count = 0;
    if (!PyArg_ParseTuple(args, "i", &frame_count)) {
        return NULL;
    }
    if (frame_count < 1) {
        PyErr_SetString(PyExc_ValueError, "frame_count must be positive");
        return NULL;
    }
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    size_t requested = (size_t)frame_count * self->bytes_per_frame;

    pthread_mutex_lock(&self->lock);
    size_t copied = tachy_min_size(requested, self->ring_size);
    copied -= copied % self->bytes_per_frame;
    if (copied < requested) {
        self->underruns += 1;
    }
    PyObject *result = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)copied);
    if (result != NULL && copied > 0) {
        char *target = PyBytes_AS_STRING(result);
        (void)tachy_miniaudio_input_ring_copy_out(self, (uint8_t *)target, copied);
    }
    pthread_mutex_unlock(&self->lock);

    return result;
}

static PyObject *tachy_input_flush(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    if (self->closed || !self->device_initialized) {
        PyErr_SetString(PyExc_RuntimeError, "input stream is closed");
        return NULL;
    }

    pthread_mutex_lock(&self->lock);
    self->ring_read = 0;
    self->ring_write = 0;
    self->ring_size = 0;
    pthread_mutex_unlock(&self->lock);

    Py_RETURN_NONE;
}

static PyObject *tachy_input_stats(TachyInputStream *self, PyObject *Py_UNUSED(ignored))
{
    pthread_mutex_lock(&self->lock);
    ma_uint64 frames_processed = self->frames_processed;
    ma_uint32 underruns = self->underruns;
    ma_uint32 overruns = self->overruns;
    ma_uint64 queued_frames = 0;
    double estimated_latency = 0.0;
    if (self->sample_rate > 0 && self->bytes_per_frame > 0) {
        queued_frames = (ma_uint64)(self->ring_size / self->bytes_per_frame);
        estimated_latency = (double)queued_frames / self->sample_rate;
    }
    double queued_latency = estimated_latency;
    ma_uint32 buffer_size = self->buffer_frames;
    pthread_mutex_unlock(&self->lock);

    return tachy_build_stream_stats_without_hardware_latency(
        frames_processed,
        underruns,
        overruns,
        queued_frames,
        queued_latency,
        buffer_size);
}

static PyMethodDef tachy_input_methods[] = {
    {"start", (PyCFunction)tachy_input_start, METH_NOARGS, "Start input capture."},
    {"stop", (PyCFunction)tachy_input_stop, METH_NOARGS, "Stop input capture."},
    {"close", (PyCFunction)tachy_input_close, METH_NOARGS, "Close input capture."},
    {"read", (PyCFunction)tachy_input_read, METH_VARARGS, "Read available interleaved float32 frames."},
    {"flush", (PyCFunction)tachy_input_flush, METH_NOARGS, "Discard captured frames."},
    {"stats", (PyCFunction)tachy_input_stats, METH_NOARGS, "Return stream statistics."},
    {NULL, NULL, 0, NULL}
};

static PyTypeObject TachyInputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.InputStream",
    .tp_basicsize = sizeof(TachyInputStream),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor)tachy_input_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Native miniaudio input stream.",
    .tp_methods = tachy_input_methods,
    .tp_new = tachy_input_new,
};

static void tachy_miniaudio_id_to_string(
    ma_backend backend,
    const ma_device_id *id,
    const char *kind,
    char *buffer,
    size_t buffer_size)
{
    const uint8_t *bytes = (const uint8_t *)id;
    const char *backend_id = NULL;
    size_t offset = 0;

    if (buffer_size == 0) {
        return;
    }

    switch (backend) {
        case ma_backend_alsa:
            backend_id = id->alsa;
            break;
        case ma_backend_pulseaudio:
            backend_id = id->pulse;
            break;
        case ma_backend_jack:
            (void)snprintf(buffer, buffer_size, "%s-jack-%d", kind, id->jack);
            return;
        case ma_backend_sndio:
            backend_id = id->sndio;
            break;
        case ma_backend_audio4:
            backend_id = id->audio4;
            break;
        case ma_backend_oss:
            backend_id = id->oss;
            break;
        default:
            break;
    }

    if (backend_id != NULL && backend_id[0] != '\0') {
        (void)snprintf(buffer, buffer_size, "%s-%s", kind, backend_id);
        return;
    }

    offset = (size_t)snprintf(buffer, buffer_size, "%s-", kind);
    if (offset >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
        return;
    }

    for (size_t index = 0; index < sizeof(*id) && offset + 2 < buffer_size; index++) {
        offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "%02x", bytes[index]);
    }
}

static ma_uint32 tachy_miniaudio_device_channels(const ma_device_info *info)
{
    ma_uint32 channels = 0;

    for (ma_uint32 index = 0; index < info->nativeDataFormatCount; index++) {
        ma_uint32 format_channels = info->nativeDataFormats[index].channels;
        if (format_channels > channels) {
            channels = format_channels;
        }
    }

    if (channels == 0) {
        channels = 2;
    }

    return channels;
}

static ma_uint32 tachy_miniaudio_device_sample_rate(const ma_device_info *info)
{
    for (ma_uint32 index = 0; index < info->nativeDataFormatCount; index++) {
        ma_uint32 sample_rate = info->nativeDataFormats[index].sampleRate;
        if (sample_rate > 0) {
            return sample_rate;
        }
    }

    return 0;
}

static int tachy_miniaudio_append_device(
    PyObject *devices,
    ma_backend backend,
    const ma_device_info *info,
    const char *kind)
{
    char id[sizeof(ma_device_id) * 2 + 16] = {0};
    const char *name = info->name[0] != '\0' ? info->name : "Audio Device";
    PyObject *entry = NULL;
    int ok = 0;

    tachy_miniaudio_id_to_string(backend, &info->id, kind, id, sizeof(id));

    entry = Py_BuildValue(
        "{s:s,s:s,s:s,s:I,s:I,s:O}",
        "id", id,
        "name", name,
        "kind", kind,
        "channels", tachy_miniaudio_device_channels(info),
        "default_sample_rate", tachy_miniaudio_device_sample_rate(info),
        "is_default", info->isDefault ? Py_True : Py_False
    );

    if (entry == NULL) {
        return 0;
    }

    ok = PyList_Append(devices, entry) == 0;
    Py_DECREF(entry);
    return ok;
}

static PyObject *tachy_list_devices(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;

    ma_context context;
    ma_result result = tachy_miniaudio_context_init(&context);
    if (result != MA_SUCCESS) {
        return PyList_New(0);
    }

    ma_device_info *playback_infos = NULL;
    ma_device_info *capture_infos = NULL;
    ma_uint32 playback_count = 0;
    ma_uint32 capture_count = 0;
    result = ma_context_get_devices(
        &context,
        &playback_infos,
        &playback_count,
        &capture_infos,
        &capture_count);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&context);
        return PyList_New(0);
    }

    PyObject *devices = PyList_New(0);
    if (devices == NULL) {
        ma_context_uninit(&context);
        return NULL;
    }

    for (ma_uint32 index = 0; index < playback_count; index++) {
        if (!tachy_miniaudio_append_device(devices, context.backend, &playback_infos[index], "output")) {
            Py_DECREF(devices);
            ma_context_uninit(&context);
            return NULL;
        }
    }

    for (ma_uint32 index = 0; index < capture_count; index++) {
        if (!tachy_miniaudio_append_device(devices, context.backend, &capture_infos[index], "input")) {
            Py_DECREF(devices);
            ma_context_uninit(&context);
            return NULL;
        }
    }

    ma_context_uninit(&context);
    return devices;
}

typedef struct {
    PyObject_HEAD
} TachyDuplexStream;

static PyObject *tachy_duplex_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    (void)type;
    (void)args;
    (void)kwargs;
    PyErr_SetString(PyExc_RuntimeError, "native duplex streams are not available on this platform");
    return NULL;
}

static PyTypeObject TachyDuplexStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.DuplexStream",
    .tp_basicsize = sizeof(TachyDuplexStream),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Unavailable native duplex stream.",
    .tp_new = tachy_duplex_new,
};

#else

typedef struct {
    PyObject_HEAD
} TachyOutputStream;

typedef struct {
    PyObject_HEAD
} TachyInputStream;

static PyObject *tachy_output_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    (void)type;
    (void)args;
    (void)kwargs;
    PyErr_SetString(PyExc_RuntimeError, "native output streams are not available on this platform");
    return NULL;
}

static PyTypeObject TachyOutputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.OutputStream",
    .tp_basicsize = sizeof(TachyOutputStream),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Unavailable native output stream.",
    .tp_new = tachy_output_new,
};

static PyObject *tachy_input_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    (void)type;
    (void)args;
    (void)kwargs;
    PyErr_SetString(PyExc_RuntimeError, "native input streams are not available on this platform");
    return NULL;
}

static PyTypeObject TachyInputStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.InputStream",
    .tp_basicsize = sizeof(TachyInputStream),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Unavailable native input stream.",
    .tp_new = tachy_input_new,
};

typedef struct {
    PyObject_HEAD
} TachyDuplexStream;

static PyObject *tachy_duplex_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    (void)type;
    (void)args;
    (void)kwargs;
    PyErr_SetString(PyExc_RuntimeError, "native duplex streams are not available on this platform");
    return NULL;
}

static PyTypeObject TachyDuplexStreamType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "tachyaudio._native.DuplexStream",
    .tp_basicsize = sizeof(TachyDuplexStream),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Unavailable native duplex stream.",
    .tp_new = tachy_duplex_new,
};

static PyObject *tachy_list_devices(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;
    return PyList_New(0);
}

#endif

static PyObject *tachy_backend_name(PyObject *self, PyObject *args)
{
    (void)self;
    (void)args;

#ifdef __APPLE__
    return PyUnicode_FromString("coreaudio");
#elif defined(__linux__)
    return PyUnicode_FromString("miniaudio");
#else
    return PyUnicode_FromString("native-unavailable");
#endif
}

static PyMethodDef tachy_methods[] = {
    {"backend_name", tachy_backend_name, METH_NOARGS, "Return the active native backend name."},
    {"list_devices", tachy_list_devices, METH_NOARGS, "List native audio devices."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef tachy_module = {
    PyModuleDef_HEAD_INIT,
    "_native",
    "Native tachyaudio backend.",
    -1,
    tachy_methods
};

PyMODINIT_FUNC PyInit__native(void)
{
    PyObject *module = NULL;

    if (PyType_Ready(&TachyOutputStreamType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&TachyInputStreamType) < 0) {
        return NULL;
    }
    if (PyType_Ready(&TachyDuplexStreamType) < 0) {
        return NULL;
    }

    module = PyModule_Create(&tachy_module);
    if (module == NULL) {
        return NULL;
    }

    Py_INCREF(&TachyOutputStreamType);
    if (PyModule_AddObject(module, "OutputStream", (PyObject *)&TachyOutputStreamType) < 0) {
        Py_DECREF(&TachyOutputStreamType);
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&TachyInputStreamType);
    if (PyModule_AddObject(module, "InputStream", (PyObject *)&TachyInputStreamType) < 0) {
        Py_DECREF(&TachyInputStreamType);
        Py_DECREF(module);
        return NULL;
    }

    Py_INCREF(&TachyDuplexStreamType);
    if (PyModule_AddObject(module, "DuplexStream", (PyObject *)&TachyDuplexStreamType) < 0) {
        Py_DECREF(&TachyDuplexStreamType);
        Py_DECREF(module);
        return NULL;
    }

    return module;
}
