#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*ndl_log_fn)(const char *what, int code, long long num, const char *text);
int ndl_open(const char *app_id, int width, int height, int hevc, ndl_log_fn log);
int ndl_play_video(void *data, unsigned size, long long pts_ns);
int ndl_play_audio(void *data, unsigned size, long long pts_ns);
int ndl_audio_free(void); /* bytes free in the audio buffer, -1 if unknown */
int ndl_audio_queued_ms(void); /* sound waiting in the TV's buffer */
int ndl_video_queue(void); /* frames waiting to be shown, -1 if unknown */
const char *ndl_error(void);
void ndl_close(void);
#ifdef __cplusplus
}
#endif
