/* NDL DirectMedia wrapper in C: the TV's NDL headers only compile as C. */
#define NDL_DIRECTMEDIA_API_VERSION 2
#include <libndl-media/NDL_directmedia_v2.h>
#include <string.h>
#include "ndl.h"

static ndl_log_fn s_log;

static void on_load(int type, long long num, const char *str)
{
    if (s_log) s_log("event", type, num, str ? str : "");
}

int ndl_open(const char *app_id, int width, int height, int hevc, ndl_log_fn log)
{
    if (log) s_log = log;
    int r = NDL_DirectMediaInit(app_id, NULL);
    if (s_log) s_log("init", r, 0, r ? NDL_DirectMediaGetError() : "");
    if (r) return r;
    r = NDL_DirectMediaSetAppState(NDL_DIRECTMEDIA_APP_STATE_FOREGROUND);
    if (s_log) s_log("foreground", r, 0, "");
    NDL_DIRECTMEDIA_DATA_INFO_T info;
    memset(&info, 0, sizeof info);
    info.video.width = width;
    info.video.height = height;
    info.video.type = hevc ? NDL_VIDEO_TYPE_H265 : NDL_VIDEO_TYPE_H264;
    /* On webOS 5+ NDL takes PCM or Opus audio, so Relay decodes the
     * channel's AAC/AC3 itself and sends 48 kHz stereo PCM. */
    info.audio.pcm.type = NDL_AUDIO_TYPE_PCM;
    info.audio.pcm.format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE;
    info.audio.pcm.layout = "interleaved";
    info.audio.pcm.channelMode = NDL_DIRECTMEDIA_AUDIO_PCM_MODE_STEREO;
    info.audio.pcm.sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_48KHZ;
    r = NDL_DirectMediaLoad(&info, on_load);
    if (s_log) s_log("load", r, 0, r ? NDL_DirectMediaGetError() : "");
    if (r) NDL_DirectMediaQuit();
    return r;
}

int ndl_play_video(void *data, unsigned size, long long pts_ns)
{
    return NDL_DirectVideoPlay(data, size, pts_ns);
}

int ndl_play_audio(void *data, unsigned size, long long pts_ns)
{
    return NDL_DirectAudioPlay(data, size, pts_ns);
}

int ndl_audio_free(void)
{
    int avail = 0;
    if (NDL_DirectAudioGetAvailableBufferSize(&avail) != 0) return -1;
    return avail;
}

int ndl_audio_queued_ms(void)
{
    int avail = 0, total = 0;
    if (NDL_DirectAudioGetAvailableBufferSize(&avail) != 0 || NDL_DirectAudioGetTotalBufferSize(&total) != 0) return -1;
    return (total - avail) / 192; /* 48 kHz x 2 ch x 2 bytes = 192 bytes per ms */
}

int ndl_video_queue(void)
{
    int len = 0;
    if (NDL_DirectVideoGetRenderBufferLength(&len) != 0) return -1;
    return len;
}

const char *ndl_error(void) { return NDL_DirectMediaGetError(); }

void ndl_close(void)
{
    NDL_DirectMediaUnload();
    NDL_DirectMediaQuit();
}
