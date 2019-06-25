#ifndef __YT_WEBRTC_NS_H__
#define __YT_WEBRTC_NS_H__

#ifdef __cplusplus
extern "C" {
#endif

#define YT_WEBRTC_NS_USE_HIGH_PASS_FILER (1)

extern int yt_webrtc_ns_create(unsigned int sample_rate, unsigned int channel_count,
                                                  unsigned int sample_per_frame, int policy, void **pp_hnd);
extern int yt_webrtc_ns_destroy(void *hnd);
extern int yt_webrtc_ns_frame_handle(void *hnd, const char *src_frame, char *dst_frame);

extern int yt_webrtc_nsx_create(unsigned int sample_rate, unsigned int channel_count,
                                                    unsigned int sample_per_frame, int policy, void **pp_hnd);
extern int yt_webrtc_nsx_destroy(void *hnd);
extern int yt_webrtc_nsx_frame_handle(void *hnd, const char *src_frame, char *dst_frame);

#ifdef __cplusplus
}
#endif

#endif