#include <stdlib.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    unsigned int rate;
    unsigned long written;
    uint64_t start;
    int running;
} FakePcm;

typedef struct {
    struct timespec timestamp;
    long delay;
    unsigned long available;
    int state;
} FakeStatus;

static uint64_t now_ns(void)
{
    struct timespec stamp;
    clock_gettime(CLOCK_MONOTONIC, &stamp);
    return (uint64_t)stamp.tv_sec * 1000000000ULL + stamp.tv_nsec;
}

int snd_pcm_open(void **target, const char *name, int stream, int mode)
{
    (void)name; (void)stream; (void)mode;
    *target = calloc(1, sizeof(FakePcm));
    return *target ? 0 : -1;
}
int snd_pcm_close(void *pcm) { free(pcm); return 0; }
int snd_pcm_set_params(void *pcm, int format, int access, unsigned int channels,
                       unsigned int rate, int resample, unsigned int latency)
{
    (void)access; (void)channels; (void)resample; (void)latency;
    if (getenv("TACHY_FAKE_S16") && (format == 14 || format == 10)) return -1;
    if (getenv("TACHY_FAKE_S32") && format == 14) return -1;
    ((FakePcm *)pcm)->rate = rate;
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
    (void)data;
    FakePcm *device = pcm;
    if (device->running && getenv("TACHY_FAKE_PARTIAL")) return frames - 1;
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
    clock_gettime(CLOCK_MONOTONIC, &snapshot->timestamp);
    uint64_t now = (uint64_t)snapshot->timestamp.tv_sec * 1000000000ULL + snapshot->timestamp.tv_nsec;
    unsigned long played = (unsigned long)((now - device->start) * device->rate / 1000000000ULL);
    snapshot->delay = (long)device->written - (long)played;
    snapshot->available = snapshot->delay < 0 ? 1921 : 1920 - snapshot->delay;
    snapshot->state = device->running && snapshot->delay >= 0 ? 3 : 4;
    if (getenv("TACHY_FAKE_CLOCK")) snapshot->timestamp.tv_sec = 0;
    return 0;
}
int snd_pcm_status_get_state(const void *status) { return ((const FakeStatus *)status)->state; }
unsigned long snd_pcm_status_get_avail(const void *status) { return ((const FakeStatus *)status)->available; }
long snd_pcm_status_get_delay(const void *status) { return ((const FakeStatus *)status)->delay; }
void snd_pcm_status_get_htstamp(const void *status, struct timespec *stamp)
{ *stamp = ((const FakeStatus *)status)->timestamp; }
