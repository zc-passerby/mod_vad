#ifndef __YUNFAN_WEBRTC_MEDIA_COMM_H__
#define __YUNFAN_WEBRTC_MEDIA_COMM_H__

#ifdef __cplusplus
extern "C" {
#endif

// boolean
#ifdef WIN32
#   ifndef __cplusplus
#       define bool char
#       define true 1
#       define false 0
#   endif
#else
#   ifndef __cplusplus
#       include <stdbool.h>
#   endif
#endif

#define NATIVE_AUDIO_ERR_SUCCESS (0)
#define NATIVE_AUDIO_ERR_INVALID_PARAM (-1)
#define NATIVE_AUDIO_ERR_NOENOUGH_MEMORY (-2)
#define NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL (-3)

enum webrtc_vad_context_mold_t
{
    WEBRTC_VAD_CORRELATED = 1,
    WEBRTC_VAD_UNCORRELATED,
    WEBRTC_VAD_MIXTURE,
};

enum webrtc_ns_policy_t
{
    WEBRTC_NS_MILD = 0,
    WEBRTC_NS_MEDIUMN,
    WEBRTC_NS_AGGRESSIVE,
    WEBRTC_NS_HIGHEST,
    WEBRTC_NS_INVALID,
};

enum webrtc_ns_mold_t
{
    WEBRTC_NS_MOLD = 0,
    WEBRTC_NSX_MOLD,
    WEBRTC_MOLD_INVALID,
};

extern int media_copy_samples(short *dst, const short *src, unsigned int count);

#ifdef __cplusplus
}
#endif

#endif
