#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    unsigned int rate, channels;
    unsigned long written;
    uint64_t start;
    int running;
    long long onset_frame;
    unsigned int seed;
    uint64_t last_status;
    int stereo_only, s16, s32, partial, usb, bad_clock, float_data;
} FakePcm;

/* Modes for the next snd_pcm_open, set with tachy_fake_set_modes() and copied
 * into the device at open. Deliberately not environment variables: the worker
 * thread must never call getenv() while a test rewrites os.environ (setenv
 * racing getenv segfaults). */
enum {
    FAKE_STEREO_ONLY = 1,
    FAKE_S16 = 2,
    FAKE_S32 = 4,
    FAKE_PARTIAL = 8,
    FAKE_USB = 16,
    FAKE_BAD_CLOCK = 32
};
static int fake_next_modes;
/* Longest interval between worker status reads, across devices, since reset. */
static uint64_t fake_max_status_gap;

typedef struct {
    struct timespec timestamp;
    long delay;
    unsigned long available;
    int state;
} FakeStatus;

/* USB-like mode: frames in flight beyond the ring, +/-1 ms delay noise, and
 * hardware timestamps that only advance every 4 ms. */
#define FAKE_USB_IN_FLIGHT 240
#define FAKE_USB_NOISE 48
#define FAKE_USB_STAMP_NS 4000000ULL

static FakePcm *fake_last;

static uint64_t now_ns(void)
{
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return (uint64_t)stamp.tv_sec * 1000000000ULL + stamp.tv_nsec;
}

int snd_pcm_open(void **target, const char *name, int stream, int mode)
{
    (void)name; (void)stream; (void)mode;
    FakePcm *device = calloc(1, sizeof(FakePcm));
    if (!device) return -1;
    device->onset_frame = -1;
    device->seed = 1;
    int modes = __atomic_load_n(&fake_next_modes, __ATOMIC_ACQUIRE);
    device->stereo_only = (modes & FAKE_STEREO_ONLY) != 0;
    device->s16 = (modes & FAKE_S16) != 0;
    device->s32 = (modes & FAKE_S32) != 0;
    device->partial = (modes & FAKE_PARTIAL) != 0;
    device->usb = (modes & FAKE_USB) != 0;
    device->bad_clock = (modes & FAKE_BAD_CLOCK) != 0;
    *target = fake_last = device;
    return 0;
}
int snd_pcm_close(void *pcm) { if (pcm == fake_last) fake_last = NULL; free(pcm); return 0; }
int snd_pcm_set_params(void *pcm, int format, int access, unsigned int channels,
                       unsigned int rate, int resample, unsigned int latency)
{
    (void)access; (void)resample; (void)latency;
    FakePcm *device = pcm;
    if (device->stereo_only && channels != 2) return -1;
    if (device->s16 && (format == 14 || format == 10)) return -1;
    if (device->s32 && format == 14) return -1;
    device->rate = rate;
    device->channels = channels;
    device->float_data = format == 14;
    return 0;
}
int snd_pcm_get_params(void *pcm, unsigned long *capacity, unsigned long *period)
{
    (void)pcm; *capacity = 1920; *period = 480; return 0;
}
int snd_pcm_prepare(void *pcm)
{
    ((FakePcm *)pcm)->written = 0; ((FakePcm *)pcm)->running = 0; return 0;
}
int snd_pcm_start(void *pcm) { (void)pcm; return 0; }
int snd_pcm_drop(void *pcm) { ((FakePcm *)pcm)->running = 0; return 0; }
long snd_pcm_writei(void *pcm, const void *data, unsigned long frames)
{
    FakePcm *device = pcm;
    if (device->running && device->partial) return frames - 1;
    if (device->onset_frame < 0 && device->float_data) {
        const float *samples = data;
        for (unsigned long index = 0; index < frames * device->channels; index++) {
            if (samples[index] != 0) {
                device->onset_frame = (long long)(device->written + index / device->channels);
                break;
            }
        }
    }
    device->written += frames;
    if (!device->running) { device->start = now_ns(); device->running = 1; }
    return frames;
}
int snd_pcm_sw_params_malloc(void **params) { *params = malloc(1); return *params ? 0 : -1; }
void snd_pcm_sw_params_free(void *params) { free(params); }
int snd_pcm_sw_params_current(void *pcm, void *params) { (void)pcm; (void)params; return 0; }
int snd_pcm_sw_params_set_start_threshold(void *pcm, void *params, unsigned long value)
{ (void)pcm; (void)params; (void)value; return 0; }
int snd_pcm_sw_params_set_tstamp_mode(void *pcm, void *params, int value)
{ (void)pcm; (void)params; (void)value; return 0; }
int snd_pcm_sw_params_set_tstamp_type(void *pcm, void *params, int value)
{ (void)pcm; (void)params; return value == 1 ? 0 : -1; }
int snd_pcm_sw_params(void *pcm, void *params) { (void)pcm; (void)params; return 0; }
int snd_pcm_status_malloc(void **status) { *status = calloc(1, sizeof(FakeStatus)); return *status ? 0 : -1; }
void snd_pcm_status_free(void *status) { free(status); }
int snd_pcm_status(void *pcm, void *status)
{
    FakePcm *device = pcm;
    FakeStatus *snapshot = status;
    int usb = device->usb;
    uint64_t now = now_ns();
    if (device->running && device->last_status) {
        uint64_t gap = now - device->last_status;
        uint64_t longest = __atomic_load_n(&fake_max_status_gap, __ATOMIC_RELAXED);
        while (gap > longest && !__atomic_compare_exchange_n(&fake_max_status_gap, &longest, gap, 0,
                                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
    }
    device->last_status = now;
    long played = (long)((now - device->start) * device->rate / 1000000000ULL);
    if (usb) played -= FAKE_USB_IN_FLIGHT;
    long queued = (long)device->written - played - (usb ? FAKE_USB_IN_FLIGHT : 0);
    snapshot->delay = (long)device->written - played;
    if (usb) snapshot->delay += (long)(rand_r(&device->seed) % (2 * FAKE_USB_NOISE + 1)) - FAKE_USB_NOISE;
    snapshot->available = queued < 0 ? 1921 : 1920 - (unsigned long)queued;
    snapshot->state = device->running && queued >= 0 ? 3 : 4;
    if (usb && now > device->start) now -= (now - device->start) % FAKE_USB_STAMP_NS;
    snapshot->timestamp.tv_sec = (time_t)(now / 1000000000ULL);
    snapshot->timestamp.tv_nsec = (long)(now % 1000000000ULL);
    if (device->bad_clock) snapshot->timestamp.tv_sec = 0;
    return 0;
}
int snd_pcm_status_get_state(const void *status) { return ((const FakeStatus *)status)->state; }
unsigned long snd_pcm_status_get_avail(const void *status) { return ((const FakeStatus *)status)->available; }
long snd_pcm_status_get_delay(const void *status) { return ((const FakeStatus *)status)->delay; }
void snd_pcm_status_get_htstamp(const void *status, struct timespec *stamp)
{ *stamp = ((const FakeStatus *)status)->timestamp; }

/* Test hook: modes (FAKE_* bits) for devices opened from now on. */
void tachy_fake_set_modes(int modes)
{
    __atomic_store_n(&fake_next_modes, modes, __ATOMIC_RELEASE);
}

/* Test hooks: longest interval between worker status reads since the last
 * reset. Only workers call snd_pcm_status, so this is how long the host left a
 * worker unscheduled (normally ~1 ms: one sleep plus a status read). */
long long tachy_fake_max_status_gap_ns(void)
{
    return (long long)__atomic_load_n(&fake_max_status_gap, __ATOMIC_RELAXED);
}

void tachy_fake_reset_max_status_gap(void)
{
    __atomic_store_n(&fake_max_status_gap, 0, __ATOMIC_RELAXED);
}

/* Test hook: true monotonic time at which the first non-silent frame plays. */
long long tachy_fake_onset_ns(void)
{
    if (!fake_last || fake_last->onset_frame < 0) return -1;
    long long frames = fake_last->onset_frame;
    if (fake_last->usb) frames += FAKE_USB_IN_FLIGHT;
    return (long long)fake_last->start + frames * 1000000000LL / fake_last->rate;
}
