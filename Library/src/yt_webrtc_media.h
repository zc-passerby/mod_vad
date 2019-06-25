#ifndef __YT_WEBRTC_MEDIA_H_
#define __YT_WEBRTC_MEDIA_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yt_vad_session_t yt_vad_session;

yt_vad_session * yt_webrtc_create_vad_session(int vad_agn, int vad_frame_time_width,
                                                    int vad_correlate, int pre_ns, int ns_agn, int sample_rate,
                                                    int check_frame_time_width, int slide_window_time_width);

void yt_webrtc_destroy_vad_session(yt_vad_session *session);

int yt_webrtc_vad_operator(yt_vad_session *session, void *audio_stream, unsigned int audio_stream_length, int *result);

int yt_webrtc_vad_res_length(yt_vad_session *session);

#ifdef __cplusplus
}
#endif

#endif