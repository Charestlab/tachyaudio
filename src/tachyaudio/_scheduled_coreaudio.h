#include <stdatomic.h>
#include <math.h>

#include "_scheduled_common.h"

#define TACHY_CORE_QUEUE 32
typedef struct {
    float *samples;
    size_t frame_count, position;
    int immediate;
    UInt64 id, requested_ns, predicted_onset_ns, render_callback_ns;
    UInt64 render_complete_ns, predicted_end_ns, decision_buffer_ns;
    double expected_sample_time;
    atomic_int state, cancel_requested;
    atomic_ullong rendered_frames;
} TachyCoreStimulus;

typedef struct {
    PyObject_HEAD
    AudioObjectID device;
    AudioDeviceIOProcID proc;
    mach_timebase_info_data_t timebase;
    double sample_rate;
    UInt32 channels, device_channels, buffer_frames, original_buffer_frames;
    UInt32 minimum_buffer_frames, maximum_buffer_frames;
    int buffer_changed;
    int running, busy, closed, listener_installed;
    AudioStreamID *streams;
    UInt32 stream_count, stream_listeners;
    UInt64 next_id, latest_id, presentation_latency_ns;
    TachyCoreStimulus stimuli[TACHY_CORE_QUEUE];
    atomic_int fault, device_changed, latency_changed;
    atomic_ullong callbacks, observations, delay_ns, max_delay_ns;
    double expected_sample_time;
    int clock_valid;
} TachyScheduledOutput;

static int tachy_core_active(int state)
{
    return state == TACHY_SCHEDULE_PENDING || state == TACHY_SCHEDULE_PLAYING;
}

static void tachy_core_init(TachyScheduledOutput *self)
{
    mach_timebase_info(&self->timebase);
    atomic_init(&self->fault, 0);
    atomic_init(&self->device_changed, 0);
    atomic_init(&self->latency_changed, 0);
    atomic_init(&self->callbacks, 0);
    atomic_init(&self->observations, 0);
    atomic_init(&self->delay_ns, 0);
    atomic_init(&self->max_delay_ns, 0);
    for (int i = 0; i < TACHY_CORE_QUEUE; i++) {
        atomic_init(&self->stimuli[i].state, TACHY_SCHEDULE_IDLE);
        atomic_init(&self->stimuli[i].cancel_requested, 0);
        atomic_init(&self->stimuli[i].rendered_frames, 0);
    }
}

static OSStatus tachy_scheduled_device_changed(AudioObjectID device, UInt32 count,
    const AudioObjectPropertyAddress addresses[], void *context)
{
    (void)device;
    TachyScheduledOutput *self = context;
    /* Latency/buffer changes need a refresh, not a fatal clock reset. */
    for (UInt32 i = 0; i < count; i++) {
        AudioObjectPropertySelector selector = addresses[i].mSelector;
        if (selector == kAudioDevicePropertyLatency ||
            selector == kAudioStreamPropertyLatency ||
            selector == kAudioDevicePropertyBufferFrameSize)
            atomic_store_explicit(&self->latency_changed, 1, memory_order_release);
        else
            atomic_store_explicit(&self->device_changed, 1, memory_order_release);
    }
    if (!count)
        atomic_store_explicit(&self->device_changed, 1, memory_order_release);
    return noErr;
}

static const AudioObjectPropertySelector tachy_core_properties[] = {
    kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyBufferFrameSize,
    kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyLatency
};

static void tachy_scheduled_remove_listener(TachyScheduledOutput *self)
{
    for (int i = 0; i < self->listener_installed; i++) {
        AudioObjectPropertyAddress address = {tachy_core_properties[i],
            (i == 3 ? kAudioDevicePropertyScopeOutput : kAudioObjectPropertyScopeGlobal), kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(self->device, &address, tachy_scheduled_device_changed, self);
    }
    self->listener_installed = 0;
    AudioObjectPropertyAddress address = {kAudioStreamPropertyVirtualFormat,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    for (UInt32 i = 0; i < self->stream_listeners; i++) {
        address.mSelector = i % 2 ? kAudioStreamPropertyLatency : kAudioStreamPropertyVirtualFormat;
        AudioObjectRemovePropertyListener(self->streams[i / 2], &address, tachy_scheduled_device_changed, self);
    }
    self->stream_listeners = 0;
}

static void tachy_core_restore_buffer(TachyScheduledOutput *self)
{
    if (!self->buffer_changed) return;
    AudioObjectPropertyAddress address = {kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 current = 0, size = sizeof(current);
    if (AudioObjectGetPropertyData(self->device, &address, 0, NULL, &size, &current) == noErr &&
        current == self->buffer_frames)
        AudioObjectSetPropertyData(self->device, &address, 0, NULL,
                                  sizeof(self->original_buffer_frames), &self->original_buffer_frames);
    self->buffer_changed = 0;
}

static UInt64 tachy_scheduled_ns(TachyScheduledOutput *self, UInt64 ticks)
{
    return (ticks / self->timebase.denom) * self->timebase.numer +
        ((ticks % self->timebase.denom) * self->timebase.numer) / self->timebase.denom;
}


static OSStatus tachy_core_render_one(
    AudioObjectID device, const AudioTimeStamp *now,
    const AudioBufferList *input, const AudioTimeStamp *input_time,
    AudioBufferList *output, const AudioTimeStamp *output_time, void *context, TachyCoreStimulus *slot)
{
    (void)device;
    (void)now;
    (void)input;
    (void)input_time;
    TachyScheduledOutput *self = context;
    int state = atomic_load_explicit(&slot->state, memory_order_acquire);
    int next_state = state;
    if (state != TACHY_SCHEDULE_PENDING && state != TACHY_SCHEDULE_PLAYING) {
        return next_state;
    }
    if (atomic_load_explicit(&self->device_changed, memory_order_acquire)) {
        next_state = TACHY_SCHEDULE_DISCONTINUITY;
        return TACHY_SCHEDULE_DISCONTINUITY;
    }
    if (atomic_load_explicit(&slot->cancel_requested, memory_order_acquire)) {
        next_state = TACHY_SCHEDULE_CANCELLED;
        return next_state;
    }
    AudioTimeStampFlags required = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
    if ((output_time->mFlags & required) != required) {
        next_state = TACHY_SCHEDULE_CLOCK_ERROR;
        return TACHY_SCHEDULE_CLOCK_ERROR;
    }
    UInt32 frame_count = 0;
    UInt32 channels = 0;
    for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
        AudioBuffer *buffer = &output->mBuffers[index];
        if (buffer->mNumberChannels == 0 || buffer->mData == NULL ||
            buffer->mDataByteSize % (buffer->mNumberChannels * sizeof(float)) != 0) {
            next_state = TACHY_SCHEDULE_FORMAT_ERROR;
            return TACHY_SCHEDULE_FORMAT_ERROR;
        }
        UInt32 frames = buffer->mDataByteSize / (buffer->mNumberChannels * sizeof(float));
        if (index == 0) {
            frame_count = frames;
        } else if (frames != frame_count) {
            next_state = TACHY_SCHEDULE_FORMAT_ERROR;
            return TACHY_SCHEDULE_FORMAT_ERROR;
        }
        channels += buffer->mNumberChannels;
    }
    if (frame_count == 0 || channels != self->device_channels) {
        next_state = TACHY_SCHEDULE_FORMAT_ERROR;
        return TACHY_SCHEDULE_FORMAT_ERROR;
    }
    // HAL delivery timestamps exclude presentation latency.
    UInt64 beginning = tachy_scheduled_ns(self, output_time->mHostTime) + self->presentation_latency_ns;
    UInt32 offset = 0;
    if (state == TACHY_SCHEDULE_PENDING) {
        UInt64 tick_ns = (self->timebase.numer + self->timebase.denom - 1) / self->timebase.denom;
        UInt64 target = slot->requested_ns;
        if (target > beginning) target -= target - beginning < tick_ns ? target - beginning : tick_ns;
        long long wanted_offset = slot->immediate ? 0 :
            tachy_schedule_offset(target, beginning, self->sample_rate);
        if (wanted_offset < 0) {
            slot->decision_buffer_ns = beginning;
            next_state = TACHY_SCHEDULE_MISSED;
            return next_state;
        }
        if ((UInt64)wanted_offset >= frame_count) {
            return next_state;
        }
        offset = (UInt32)wanted_offset;
        slot->decision_buffer_ns = beginning;
        slot->predicted_onset_ns = beginning + (UInt64)llround(offset * 1000000000.0 / self->sample_rate);
        slot->render_callback_ns = tachy_scheduled_ns(self, mach_absolute_time());
        if (slot->render_callback_ns >= slot->predicted_onset_ns - self->presentation_latency_ns) {
            next_state = TACHY_SCHEDULE_CALLBACK_LATE;
            return TACHY_SCHEDULE_CALLBACK_LATE;
        }
    } else if (fabs(output_time->mSampleTime - slot->expected_sample_time) > 0.5) {
        next_state = TACHY_SCHEDULE_DISCONTINUITY;
        return TACHY_SCHEDULE_DISCONTINUITY;
    }
    if (state == TACHY_SCHEDULE_PLAYING &&
        tachy_scheduled_ns(self, mach_absolute_time()) >= beginning - self->presentation_latency_ns) {
        next_state = TACHY_SCHEDULE_CALLBACK_LATE;
        return TACHY_SCHEDULE_CALLBACK_LATE;
    }
    size_t count = slot->frame_count - slot->position;
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
                // Additive mix: the output buffer is zeroed once per callback, so
                // overlapping stimuli sum together (clamped after the slot loop).
                destination[(offset + frame) * buffer->mNumberChannels + channel] +=
                    slot->samples[(slot->position + frame) * self->channels + source_channel];
            }
        }
        channel_base += buffer->mNumberChannels;
    }
    UInt64 render_complete_ns = tachy_scheduled_ns(self, mach_absolute_time());
    UInt64 first_sample_ns = beginning + (UInt64)llround(offset * 1e9 / self->sample_rate);
    if (render_complete_ns >= first_sample_ns - self->presentation_latency_ns) {
        for (UInt32 index = 0; index < output->mNumberBuffers; index++) {
            AudioBuffer *buffer = &output->mBuffers[index];
            memset((float *)buffer->mData + offset * buffer->mNumberChannels, 0,
                   count * buffer->mNumberChannels * sizeof(float));
        }
        next_state = TACHY_SCHEDULE_CALLBACK_LATE;
        return TACHY_SCHEDULE_CALLBACK_LATE;
    }
    if (state == TACHY_SCHEDULE_PENDING) {
        slot->render_complete_ns = render_complete_ns;
    }
    slot->position += count;
    slot->expected_sample_time = output_time->mSampleTime + frame_count;
    atomic_store_explicit(&slot->rendered_frames, slot->position, memory_order_relaxed);
    if (slot->position == slot->frame_count) {
        slot->predicted_end_ns = beginning +
            (UInt64)llround((offset + count) * 1000000000.0 / self->sample_rate);
        next_state = TACHY_SCHEDULE_RENDERED;
    } else if (state == TACHY_SCHEDULE_PENDING) {
        next_state = TACHY_SCHEDULE_PLAYING;
    }
    return next_state;
}

static void tachy_core_fail(TachyScheduledOutput *self, int fault, UInt64 beginning)
{
    atomic_store_explicit(&self->fault, fault, memory_order_release);
    for (int i = 0; i < TACHY_CORE_QUEUE; i++) {
        TachyCoreStimulus *slot = &self->stimuli[i];
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (tachy_core_active(state))
            atomic_store_explicit(&slot->state, fault, memory_order_release);
    }
    (void)beginning;
}

static OSStatus tachy_scheduled_render(
    AudioObjectID device, const AudioTimeStamp *now,
    const AudioBufferList *input, const AudioTimeStamp *input_time,
    AudioBufferList *output, const AudioTimeStamp *output_time, void *context)
{
    TachyScheduledOutput *self = context;
    for (UInt32 i = 0; i < output->mNumberBuffers; i++)
        if (output->mBuffers[i].mData)
            memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    atomic_fetch_add_explicit(&self->callbacks, 1, memory_order_relaxed);
    int previous_fault = atomic_load_explicit(&self->fault, memory_order_acquire);
    if (previous_fault) { tachy_core_fail(self, previous_fault, 0); return noErr; }
    int fault = 0;
    AudioTimeStampFlags required = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
    if (atomic_load_explicit(&self->device_changed, memory_order_acquire))
        fault = TACHY_SCHEDULE_DISCONTINUITY;
    else if ((output_time->mFlags & required) != required)
        fault = TACHY_SCHEDULE_CLOCK_ERROR;
    UInt32 frames = 0, channels = 0;
    for (UInt32 i = 0; i < output->mNumberBuffers && !fault; i++) {
        const AudioBuffer *buffer = &output->mBuffers[i];
        if (!buffer->mData || !buffer->mNumberChannels ||
            buffer->mDataByteSize % (buffer->mNumberChannels * sizeof(float))) {
            fault = TACHY_SCHEDULE_FORMAT_ERROR;
            break;
        }
        UInt32 count = buffer->mDataByteSize / (buffer->mNumberChannels * sizeof(float));
        if (i && count != frames) fault = TACHY_SCHEDULE_FORMAT_ERROR;
        frames = count;
        channels += buffer->mNumberChannels;
    }
    if (!frames || channels != self->device_channels) fault = TACHY_SCHEDULE_FORMAT_ERROR;
    if (!fault && self->clock_valid && fabs(output_time->mSampleTime - self->expected_sample_time) > 0.5)
        fault = TACHY_SCHEDULE_DISCONTINUITY;
    if (fault) { tachy_core_fail(self, fault, 0); return noErr; }
    self->clock_valid = 1;
    self->expected_sample_time = output_time->mSampleTime + frames;
    // HAL delivery timestamps exclude presentation latency.
    UInt64 beginning = tachy_scheduled_ns(self, output_time->mHostTime) + self->presentation_latency_ns;
    UInt64 current = tachy_scheduled_ns(self, mach_absolute_time());
    if (beginning - self->presentation_latency_ns <= current) {
        tachy_core_fail(self, TACHY_SCHEDULE_CALLBACK_LATE, beginning);
        return noErr;
    }
    UInt64 delay = beginning - current;
    atomic_store_explicit(&self->delay_ns, delay, memory_order_relaxed);
    if (delay > atomic_load_explicit(&self->max_delay_ns, memory_order_relaxed))
        atomic_store_explicit(&self->max_delay_ns, delay, memory_order_relaxed);
    atomic_fetch_add_explicit(&self->observations, 1, memory_order_release);
    int states[TACHY_CORE_QUEUE] = {0};
    int contributors = 0;
    UInt64 first_written_ns = UINT64_MAX;
    for (int i = 0; i < TACHY_CORE_QUEUE; i++) {
        TachyCoreStimulus *slot = &self->stimuli[i];
        int state = atomic_load_explicit(&slot->state, memory_order_acquire);
        if (!tachy_core_active(state)) continue;
        size_t before = slot->position;
        states[i] = tachy_core_render_one(device, now, input, input_time, output,
                                         output_time, self, slot);
        if (states[i] >= TACHY_SCHEDULE_CLOCK_ERROR) { fault = states[i]; break; }
        if (slot->position > before) {
            contributors++;
            UInt64 first = state == TACHY_SCHEDULE_PENDING ? slot->predicted_onset_ns : beginning;
            if (first < first_written_ns) first_written_ns = first;
        }
    }
    if (!fault && first_written_ns != UINT64_MAX &&
        tachy_scheduled_ns(self, mach_absolute_time()) >= first_written_ns - self->presentation_latency_ns)
        fault = TACHY_SCHEDULE_CALLBACK_LATE;
    if (fault) {
        for (UInt32 i = 0; i < output->mNumberBuffers; i++)
            memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
        tachy_core_fail(self, fault, beginning);
    } else {
        // Only overlapping stimuli can sum past [-1, 1]; clamp just when they mixed,
        // leaving single-stimulus playback exactly as it was before mixing existed.
        if (contributors > 1)
            for (UInt32 b = 0; b < output->mNumberBuffers; b++) {
                float *d = output->mBuffers[b].mData;
                UInt32 n = output->mBuffers[b].mDataByteSize / sizeof(float);
                for (UInt32 k = 0; k < n; k++)
                    d[k] = d[k] > 1.0f ? 1.0f : (d[k] < -1.0f ? -1.0f : d[k]);
            }
        for (int i = 0; i < TACHY_CORE_QUEUE; i++)
            if (states[i]) atomic_store_explicit(&self->stimuli[i].state, states[i], memory_order_release);
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
    tachy_core_restore_buffer(self);
    PyMem_RawFree(self->streams);
    for (int i = 0; i < TACHY_CORE_QUEUE; i++) PyMem_RawFree(self->stimuli[i].samples);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *tachy_scheduled_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    static char *keywords[] = {"sample_rate", "channels", "device_id", "buffer_us", NULL};
    int sample_rate = 48000;
    int channels = 2;
    const char *device_id = NULL;
    unsigned int buffer_us = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iizI", keywords,
                                    &sample_rate, &channels, &device_id, &buffer_us)) {
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
    tachy_core_init(self);
    if (!atomic_is_lock_free(&self->fault) || !atomic_is_lock_free(&self->callbacks)) {
        PyErr_SetString(PyExc_RuntimeError, "native scheduling requires lock-free atomics");
        Py_DECREF(self);
        return NULL;
    }
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
                format.mBitsPerChannel != 32 || format.mSampleRate != sample_rate ||
                format.mFramesPerPacket != 1 ||
                format.mBytesPerFrame != sizeof(float) *
                    ((format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? 1 : format.mChannelsPerFrame)) {
                result = kAudio_ParamError;
                break;
            }
            self->device_channels += format.mChannelsPerFrame;
        }
    }
    self->streams = streams;
    self->stream_count = size / sizeof(AudioStreamID);
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
    AudioValueRange range = {0};
    UInt32 range_size = sizeof(range);
    AudioObjectPropertyAddress range_address = address;
    range_address.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
    OSStatus range_result = AudioObjectGetPropertyData(self->device, &range_address, 0, NULL, &range_size, &range);
    if (range_result == noErr) {
        self->minimum_buffer_frames = (UInt32)ceil(range.mMinimum);
        self->maximum_buffer_frames = (UInt32)floor(range.mMaximum);
    }
    if (result == noErr && buffer_us) {
        self->original_buffer_frames = self->buffer_frames;
        result = range_result;
        UInt32 requested = (UInt32)ceil(sample_rate * (double)buffer_us / 1000000.0);
        if (result == noErr && (requested < range.mMinimum || requested > range.mMaximum)) {
            PyErr_Format(PyExc_ValueError, "requested latency requires %u frames; device range is %.0f..%.0f",
                         requested, range.mMinimum, range.mMaximum);
            Py_DECREF(self); return NULL;
        }
        if (result == noErr)
            result = AudioObjectSetPropertyData(self->device, &address, 0, NULL, sizeof(requested), &requested);
        if (result == noErr) {
            self->buffer_changed = 1;
            self->buffer_frames = requested;
            size = sizeof(self->buffer_frames);
            result = AudioObjectGetPropertyData(self->device, &address, 0, NULL, &size, &self->buffer_frames);
        }
    }
    if (result == noErr && !self->buffer_frames) result = kAudio_ParamError;
    if (result == noErr)
        result = AudioDeviceCreateIOProcID(self->device, tachy_scheduled_render, self, &self->proc);
    for (int i = 0; result == noErr && i < 4; i++) {
        address.mSelector = tachy_core_properties[i];
        address.mScope = i == 3 ? kAudioDevicePropertyScopeOutput : kAudioObjectPropertyScopeGlobal;
        result = AudioObjectAddPropertyListener(self->device, &address, tachy_scheduled_device_changed, self);
        if (result == noErr) self->listener_installed++;
    }
    address.mScope = kAudioObjectPropertyScopeGlobal;
    for (UInt32 i = 0; result == noErr && i < self->stream_count * 2; i++) {
        address.mSelector = i % 2 ? kAudioStreamPropertyLatency : kAudioStreamPropertyVirtualFormat;
        result = AudioObjectAddPropertyListener(self->streams[i / 2], &address, tachy_scheduled_device_changed, self);
        if (result == noErr) self->stream_listeners++;
    }
    if (result != noErr) {
        PyErr_Format(PyExc_RuntimeError, "could not create device render callback (%d)", (int)result);
        Py_DECREF(self);
        return NULL;
    }
    // A single onset estimate requires equal stream latencies.
    UInt32 device_latency = 0, stream_latency = 0;
    address.mScope = kAudioDevicePropertyScopeOutput;
    address.mSelector = kAudioDevicePropertyLatency;
    size = sizeof(device_latency);
    result = AudioObjectGetPropertyData(self->device, &address, 0, NULL, &size, &device_latency);
    for (UInt32 i = 0; result == noErr && i < self->stream_count; i++) {
        UInt32 latency = 0;
        AudioObjectPropertyAddress stream_address = {kAudioStreamPropertyLatency,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        size = sizeof(latency);
        result = AudioObjectGetPropertyData(self->streams[i], &stream_address, 0, NULL, &size, &latency);
        if (i && latency != stream_latency) result = kAudio_ParamError;
        stream_latency = latency;
    }
    if (result != noErr) {
        PyErr_SetString(PyExc_ValueError, "native scheduling requires readable, equal output stream latencies");
        Py_DECREF(self); return NULL;
    }
    self->presentation_latency_ns = (UInt64)llround(
        ((double)device_latency + stream_latency) * 1e9 / sample_rate);
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
    if (atomic_load_explicit(&self->fault, memory_order_acquire)) {
        PyErr_SetString(PyExc_RuntimeError, "scheduled stream failed; reopen it");
        return NULL;
    }
    if (!self->running) {
        self->busy = 1;
        OSStatus result;
        Py_BEGIN_ALLOW_THREADS
        result = AudioDeviceStart(self->device, self->proc);
        if (result == noErr) {
            UInt64 deadline = tachy_scheduled_ns(self, mach_absolute_time()) + 2000000000ULL;
            while (atomic_load_explicit(&self->observations, memory_order_acquire) < 3 &&
                   !atomic_load_explicit(&self->fault, memory_order_acquire) &&
                   tachy_scheduled_ns(self, mach_absolute_time()) < deadline)
                tachy_sleep_ms(1);
        }
        Py_END_ALLOW_THREADS
        self->busy = 0;
        if (result != noErr) {
            PyErr_Format(PyExc_RuntimeError, "could not start audio device (%d)", (int)result);
            return NULL;
        }
        self->running = 1;
        if (atomic_load_explicit(&self->fault, memory_order_acquire) ||
            atomic_load_explicit(&self->observations, memory_order_acquire) < 3) {
            PyErr_SetString(PyExc_RuntimeError, "Core Audio clock did not become ready; close and reopen");
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

static PyObject *tachy_scheduled_schedule(TachyScheduledOutput *self, PyObject *args)
{
    PyObject *frames;
    unsigned long long target, lead = 0;
    int immediate = 0;
    if (!PyArg_ParseTuple(args, "OK|Kp", &frames, &target, &lead, &immediate) || !tachy_scheduled_check(self))
        return NULL;
    if (!self->running || atomic_load_explicit(&self->fault, memory_order_acquire)) {
        PyErr_SetString(PyExc_RuntimeError, "stream is not running or has failed; reopen it");
        return NULL;
    }
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
    if (!copy) { PyBuffer_Release(&view); return PyErr_NoMemory(); }
    memcpy(copy, view.buf, view.len);
    PyBuffer_Release(&view);
    const char *problem = NULL;
    for (size_t i = 0; i < count * self->channels; i++)
        if (!isfinite(copy[i])) { problem = "stimulus samples must be finite"; break; }
    UInt64 now = tachy_scheduled_ns(self, mach_absolute_time());
    if (!immediate && (target <= now || target - now < lead)) problem = "insufficient advance notice after copying stimulus data";
    double sample_ns = 1e9 / self->sample_rate, tolerance = sample_ns / 2;
    TachyCoreStimulus *slot = NULL;
    for (int i = 0; i < TACHY_CORE_QUEUE && !problem; i++) {
        TachyCoreStimulus *candidate = &self->stimuli[i];
        int state = atomic_load_explicit(&candidate->state, memory_order_acquire);
        // Callback-chosen onsets cannot safely mix with queued stimuli.
        int active = tachy_core_active(state);
        if (candidate->id && active && (immediate || candidate->immediate)) {
            problem = "immediate playback requires an idle stream; wait or cancel queued stimuli";
            break;
        }
        UInt64 candidate_start = candidate->immediate && !active ?
            candidate->predicted_onset_ns : candidate->requested_ns;
        int in_flight = atomic_load_explicit(&candidate->rendered_frames, memory_order_relaxed) > 0 &&
            (double)candidate_start + candidate->frame_count * sample_ns > now;
        if (candidate->id && in_flight && (immediate || candidate->immediate)) {
            problem = "immediate playback requires an idle stream; audio is still in flight";
            break;
        }
        // Overlapping stimuli are allowed: the render callback mixes active slots.
        if (!tachy_core_active(state) &&
            !in_flight &&
            (!slot || candidate->id < slot->id)) slot = candidate;
    }
    if (!slot && !problem) problem = "too many stimuli queued (32)";
    if (problem) { PyMem_RawFree(copy); PyErr_SetString(PyExc_ValueError, problem); return NULL; }
    atomic_store_explicit(&slot->state, TACHY_SCHEDULE_PREPARING, memory_order_release);
    PyMem_RawFree(slot->samples);
    slot->samples = copy;
    slot->frame_count = count;
    slot->position = 0;
    slot->id = ++self->next_id;
    slot->requested_ns = target;
    slot->immediate = immediate;
    slot->predicted_onset_ns = slot->render_callback_ns = slot->render_complete_ns = 0;
    slot->predicted_end_ns = slot->decision_buffer_ns = 0;
    atomic_store_explicit(&slot->rendered_frames, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->cancel_requested, 0, memory_order_relaxed);
    self->latest_id = slot->id;
    atomic_store_explicit(&slot->state, TACHY_SCHEDULE_PENDING, memory_order_release);
    int fault = atomic_load_explicit(&self->fault, memory_order_acquire);
    if (fault) {
        int pending = TACHY_SCHEDULE_PENDING;
        atomic_compare_exchange_strong_explicit(&slot->state, &pending, fault,
                                                memory_order_release, memory_order_relaxed);
    }
    return PyLong_FromUnsignedLongLong(slot->id);
}

static TachyCoreStimulus *tachy_core_find(TachyScheduledOutput *self, UInt64 id)
{
    if (!id) id = self->latest_id;
    for (int i = 0; i < TACHY_CORE_QUEUE; i++)
        if (id && self->stimuli[i].id == id) return &self->stimuli[i];
    return NULL;
}

static PyObject *tachy_core_record(TachyCoreStimulus *slot)
{
    int state = slot ? atomic_load_explicit(&slot->state, memory_order_acquire) : TACHY_SCHEDULE_IDLE;
    int published = state == TACHY_SCHEDULE_PLAYING || state == TACHY_SCHEDULE_RENDERED;
    return Py_BuildValue("{s:i,s:O,s:K,s:K,s:K,s:K,s:K,s:K,s:K,s:K}",
        "state", state, "immediate", slot && slot->immediate ? Py_True : Py_False, "stimulus", slot ? slot->id : 0ULL,
        "requested_ns", slot ? slot->requested_ns : 0ULL,
        "predicted_onset_ns", published ? slot->predicted_onset_ns : 0ULL,
        "render_callback_ns", published ? slot->render_callback_ns : 0ULL,
        "render_complete_ns", published ? slot->render_complete_ns : 0ULL,
        "predicted_end_ns", state == TACHY_SCHEDULE_RENDERED ? slot->predicted_end_ns : 0ULL,
        "decision_buffer_ns", published ? slot->decision_buffer_ns : 0ULL,
        "rendered_frames", slot ? atomic_load_explicit(&slot->rendered_frames, memory_order_relaxed) : 0ULL);
}

static PyObject *tachy_scheduled_result(TachyScheduledOutput *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id)) return NULL;
    TachyCoreStimulus *slot = tachy_core_find(self, id);
    if (!slot) {
        PyErr_SetString(PyExc_LookupError, "unknown stimulus id, or its record was reused after 32 newer stimuli");
        return NULL;
    }
    return tachy_core_record(slot);
}

static PyObject *tachy_scheduled_status(TachyScheduledOutput *self, PyObject *Py_UNUSED(ignored))
{
    PyObject *record = tachy_core_record(tachy_core_find(self, 0));
    if (!record) return NULL;
    unsigned int queued = 0;
    for (int i = 0; i < TACHY_CORE_QUEUE; i++)
        queued += tachy_core_active(atomic_load_explicit(&self->stimuli[i].state, memory_order_acquire));
    PyObject *extra = Py_BuildValue("{s:K,s:I,s:I,s:d,s:s,s:s,s:s,s:I,s:I,s:K,s:d,s:d,s:d,s:d,s:I,s:I,s:O}",
        "callbacks", atomic_load_explicit(&self->callbacks, memory_order_relaxed),
        "buffer_frames", self->buffer_frames, "period_frames", self->buffer_frames,
        "sample_rate", self->sample_rate, "clock_source", "coreaudio-output-host-time",
        "device_format", "float32", "rt_method", "coreaudio-io-thread",
        "device_channels", self->device_channels, "queued", queued,
        "clock_observations", atomic_load_explicit(&self->observations, memory_order_acquire),
        "output_delay_us", atomic_load_explicit(&self->delay_ns, memory_order_relaxed) / 1000.0,
        "output_delay_max_us", atomic_load_explicit(&self->max_delay_ns, memory_order_relaxed) / 1000.0,
        "presentation_latency_us", self->presentation_latency_ns / 1000.0,
        "latency_changed", (double)atomic_load_explicit(&self->latency_changed, memory_order_acquire),
        "minimum_buffer_frames", self->minimum_buffer_frames,
        "maximum_buffer_frames", self->maximum_buffer_frames,
        "buffer_automatic", self->buffer_changed ? Py_False : Py_True);
    if (!extra || PyDict_Update(record, extra) < 0) { Py_XDECREF(extra); Py_DECREF(record); return NULL; }
    Py_DECREF(extra);
    int fault = atomic_load_explicit(&self->fault, memory_order_acquire);
    if (fault) {
        PyObject *value = PyLong_FromLong(fault);
        if (!value || PyDict_SetItemString(record, "state", value) < 0) { Py_XDECREF(value); Py_DECREF(record); return NULL; }
        Py_DECREF(value);
    }
    return record;
}

static PyObject *tachy_scheduled_cancel(TachyScheduledOutput *self, PyObject *args)
{
    unsigned long long id = 0;
    if (!PyArg_ParseTuple(args, "|K", &id) || !tachy_scheduled_check(self)) return NULL;
    for (int i = 0; i < TACHY_CORE_QUEUE; i++)
        if ((!id || self->stimuli[i].id == id) &&
            tachy_core_active(atomic_load_explicit(&self->stimuli[i].state, memory_order_acquire)))
            atomic_store_explicit(&self->stimuli[i].cancel_requested, 1, memory_order_release);
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
    if (self->running && self->proc) {
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
    if (self->proc) AudioDeviceDestroyIOProcID(self->device, self->proc);
    self->proc = NULL;
    self->closed = 1;
    tachy_core_restore_buffer(self);
    for (int i = 0; i < TACHY_CORE_QUEUE; i++)
        if (tachy_core_active(atomic_load_explicit(&self->stimuli[i].state, memory_order_acquire)))
            atomic_store_explicit(&self->stimuli[i].state, TACHY_SCHEDULE_CANCELLED, memory_order_release);
    Py_RETURN_NONE;
}

static PyObject *tachy_core_test_render(TachyScheduledOutput *self, PyObject *args)
{
    unsigned long long beginning;
    unsigned int frames, count;
    int gap = 0;
    if (!PyArg_ParseTuple(args, "KII|p", &beginning, &frames, &count, &gap)) return NULL;
    if (self->proc || !frames || frames > 4096 || count > 128) {
        PyErr_SetString(PyExc_ValueError, "test renderer requires a synthetic stream and bounded buffers");
        return NULL;
    }
    PyObject *buffers = PyList_New(0);
    if (!buffers) return NULL;
    for (unsigned int i = 0; i < count; i++) {
        PyObject *data = PyBytes_FromStringAndSize(NULL, frames * self->device_channels * sizeof(float));
        if (!data) { Py_DECREF(buffers); return NULL; }
        AudioBufferList output = {0};
        output.mNumberBuffers = 1;
        output.mBuffers[0].mNumberChannels = self->device_channels;
        output.mBuffers[0].mDataByteSize = frames * self->device_channels * sizeof(float);
        output.mBuffers[0].mData = PyBytes_AS_STRING(data);
        AudioTimeStamp stamp = {0};
        UInt64 ns = beginning + (UInt64)llround(i * frames * 1e9 / self->sample_rate);
        stamp.mHostTime = (ns / self->timebase.numer) * self->timebase.denom +
            ((ns % self->timebase.numer) * self->timebase.denom) / self->timebase.numer;
        stamp.mSampleTime = self->clock_valid ? self->expected_sample_time : 0;
        if (gap && i) stamp.mSampleTime += 10;
        stamp.mFlags = kAudioTimeStampHostTimeValid | kAudioTimeStampSampleTimeValid;
        tachy_scheduled_render(0, &stamp, NULL, NULL, &output, &stamp, self);
        int added = PyList_Append(buffers, data);
        Py_DECREF(data);
        if (added < 0) { Py_DECREF(buffers); return NULL; }
    }
    return buffers;
}

static PyMethodDef tachy_scheduled_methods[] = {
    {"_render_test", (PyCFunction)tachy_core_test_render, METH_VARARGS, "Synthetic stream rendering only."},
    {"start", (PyCFunction)tachy_scheduled_start, METH_NOARGS, "Start device callbacks."},
    {"schedule", (PyCFunction)tachy_scheduled_schedule, METH_VARARGS, "Submit a future stimulus."},
    {"result", (PyCFunction)tachy_scheduled_result, METH_VARARGS, "Read one stimulus record."},
    {"status", (PyCFunction)tachy_scheduled_status, METH_NOARGS, "Read device-clock timing estimates."},
    {"cancel", (PyCFunction)tachy_scheduled_cancel, METH_VARARGS, "Cancel at the next callback."},
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

static PyObject *tachy_core_test_stream(PyObject *module, PyObject *args)
{
    (void)module;
    int channels = 1, device_channels = 2;
    if (!PyArg_ParseTuple(args, "|ii", &channels, &device_channels)) return NULL;
    if (channels < 1 || device_channels < 1 || device_channels > 8 ||
        (channels != 1 && channels != device_channels)) {
        PyErr_SetString(PyExc_ValueError, "invalid test channel count"); return NULL;
    }
    TachyScheduledOutput *self = (TachyScheduledOutput *)TachyScheduledOutputType.tp_alloc(&TachyScheduledOutputType, 0);
    if (!self) return NULL;
    tachy_core_init(self);
    self->channels = channels;
    self->device_channels = device_channels;
    self->sample_rate = 48000;
    self->buffer_frames = 8;
    self->running = 1;
    return (PyObject *)self;
}

static PyObject *tachy_scheduled_render_test(PyObject *module, PyObject *args)
{
    (void)module;
    PyObject *samples;
    int channels;
    unsigned long long requested_ns, beginning_ns, latency_ns = 0;
    unsigned int frames, callbacks;
    int gap = 0;
    if (!PyArg_ParseTuple(args, "OiKKII|pK", &samples, &channels, &requested_ns,
                          &beginning_ns, &frames, &callbacks, &gap, &latency_ns)) {
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
    tachy_core_init(&self);
    self.channels = channels;
    self.device_channels = channels;
    self.sample_rate = 48000;
    self.presentation_latency_ns = latency_ns;
    self.latest_id = self.stimuli[0].id = 1;
    self.stimuli[0].samples = view.buf;
    self.stimuli[0].frame_count = view.len / (channels * sizeof(float));
    self.stimuli[0].requested_ns = requested_ns;
    atomic_store(&self.stimuli[0].state, TACHY_SCHEDULE_PENDING);
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
