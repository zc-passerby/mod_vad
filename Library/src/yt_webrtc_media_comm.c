#include "yt_webrtc_media_comm.h"
#include <string.h>

int media_copy_samples(short *dst, const short *src, unsigned int count)
{
    if (dst && src && count)
    {
        memcpy(dst, src, (count << 1));
        return NATIVE_AUDIO_ERR_SUCCESS;
    }
    return NATIVE_AUDIO_ERR_INVALID_PARAM;
}

