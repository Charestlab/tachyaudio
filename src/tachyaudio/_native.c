#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __APPLE__

#define TACHY_OUTPUT_BUFFER_COUNT 3
#define TACHY_INPUT_BUFFER_COUNT 3
#define TACHY_DEFAULT_BUFFER_MS 10
#define TACHY_RING_SECONDS 1

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

static size_t tachy_min_size(size_t left, size_t right)
{
    return left < right ? left : right;
}

static void tachy_ring_copy_in(TachyOutputStream *stream, const uint8_t *source, size_t byte_count)
{
    size_t first = tachy_min_size(byte_count, stream->ring_capacity - stream->ring_write);
    memcpy(stream->ring + stream->ring_write, source, first);
    memcpy(stream->ring, source + first, byte_count - first);
    stream->ring_write = (stream->ring_write + byte_count) % stream->ring_capacity;
    stream->ring_size += byte_count;
}

static void tachy_input_ring_copy_in(TachyInputStream *stream, const uint8_t *source, size_t byte_count)
{
    size_t first = tachy_min_size(byte_count, stream->ring_capacity - stream->ring_write);
    memcpy(stream->ring + stream->ring_write, source, first);
    memcpy(stream->ring, source + first, byte_count - first);
    stream->ring_write = (stream->ring_write + byte_count) % stream->ring_capacity;
    stream->ring_size += byte_count;
}

static size_t tachy_input_ring_copy_out(TachyInputStream *stream, uint8_t *target, size_t byte_count)
{
    size_t copied = tachy_min_size(byte_count, stream->ring_size);
    size_t first = tachy_min_size(copied, stream->ring_capacity - stream->ring_read);

    memcpy(target, stream->ring + stream->ring_read, first);
    memcpy(target + first, stream->ring, copied - first);
    stream->ring_read = (stream->ring_read + copied) % stream->ring_capacity;
    stream->ring_size -= copied;
    return copied;
}

static size_t tachy_ring_copy_out(TachyOutputStream *stream, uint8_t *target, size_t byte_count)
{
    size_t copied = tachy_min_size(byte_count, stream->ring_size);
    size_t first = tachy_min_size(copied, stream->ring_capacity - stream->ring_read);

    memcpy(target, stream->ring + stream->ring_read, first);
    memcpy(target + first, stream->ring, copied - first);
    stream->ring_read = (stream->ring_read + copied) % stream->ring_capacity;
    stream->ring_size -= copied;
    return copied;
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

    return Py_BuildValue(
        "{s:K,s:I,s:I,s:d,s:K,s:d,s:I}",
        "frames_processed", frames_processed,
        "underruns", underruns,
        "overruns", overruns,
        "estimated_latency", estimated_latency,
        "queued_frames", queued_frames,
        "queued_latency", queued_latency,
        "buffer_size", buffer_size
    );
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

    return Py_BuildValue(
        "{s:K,s:I,s:I,s:d,s:K,s:d,s:I}",
        "frames_processed", frames_processed,
        "underruns", underruns,
        "overruns", overruns,
        "estimated_latency", estimated_latency,
        "queued_frames", queued_frames,
        "queued_latency", queued_latency,
        "buffer_size", buffer_size
    );
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

    return module;
}
