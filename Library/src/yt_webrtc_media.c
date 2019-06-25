#include "typedefs.h"
#include "webrtc_vad.h"
#include "yt_webrtc_media_comm.h"
#include "yt_webrtc_ns.h"
#include "yt_webrtc_media.h"
#include <stdlib.h>
#include <string.h>

#if 0
#define media_debug(format, ...) fprintf(stdout, format, ## __VA_ARGS__)
#else
#define media_debug(format, ...)
#endif

typedef int (*yt_ns_create)(unsigned int sample_rate, unsigned int channel_count, 
                                unsigned int sample_per_frame, int policy, void **pp_hnd);
typedef int (*yt_ns_destroy)(void *hnd);
typedef int (*yt_ns_frame_handle)(void *hnd, const char *src_frame, char *dst_frame);

typedef struct yt_ns_opt_t
{
    yt_ns_create create;
    yt_ns_destroy destroy;
    yt_ns_frame_handle frame_handle;
} yt_ns_opt;

static const yt_ns_opt NativeWebrtcNsOpts[WEBRTC_MOLD_INVALID] = 
{
    {yt_webrtc_ns_create, yt_webrtc_ns_destroy, yt_webrtc_ns_frame_handle},
    {yt_webrtc_nsx_create, yt_webrtc_nsx_destroy, yt_webrtc_nsx_frame_handle},
};

struct yt_vad_session_t
{
    // vad config
    int sample_rate;
    int vad_agn;
    int vad_frame_time_width;
    /* 1 stream, 2 uncorrelated, 3 mixture */
    int vad_correlate;
    VadInst *vad_handle;

    // ns config
    const yt_ns_opt *ns_opt;
    void *ns_handle;

    // check frame
    char *audio_stream_buffer;
    int check_frame_bytes_width;
    int audio_stream_pos;
    int vad_res_length_per_frame;

    // slide window
    char *slide_window_buffer;
    int slide_window_bytes_width;
    int vad_res_length_per_window;

    // check result
    int *vad_check_res;
    int check_res_length;
    int check_res_pos;
};

static void yt_vad_session_destroy(yt_vad_session *vad_session)
{
    if (vad_session)
    {
        if (vad_session->vad_check_res)
        {
            free(vad_session->vad_check_res);
            vad_session->vad_check_res = NULL;
        }

        if (vad_session->audio_stream_buffer)
        {
            free(vad_session->audio_stream_buffer);
            vad_session->audio_stream_buffer = NULL;
        }

        if (vad_session->slide_window_buffer)
        {
            free(vad_session->slide_window_buffer);
            vad_session->slide_window_buffer = NULL;
        }

        if (vad_session->ns_opt && vad_session->ns_handle)
        {
            vad_session->ns_opt->destroy(vad_session->ns_handle);
            vad_session->ns_handle = NULL;
        }

        if (vad_session->vad_handle)
        {
            WebRtcVad_Free(vad_session->vad_handle);
            vad_session->vad_handle = NULL;
        }

        free(vad_session);
    }
}

static int yt_vad_res_length(const yt_vad_session *vad_session)
{
    return vad_session ? vad_session->vad_res_length_per_frame : NATIVE_AUDIO_ERR_INVALID_PARAM;
}

// 时间参数以毫秒ms为单位
static int yt_vad_session_create(int vad_agn, int vad_frame_time_width,
                                     int vad_correlate, int pre_ns, int ns_agn,
                                     int sample_rate, int check_frame_time_width,
                                     int slide_window_time_width, yt_vad_session **vad_session)
{
    yt_vad_session *session = NULL;
    int ret = NATIVE_AUDIO_ERR_SUCCESS;

    if (vad_agn < 0 || vad_agn > 3)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (vad_frame_time_width != 10 && vad_frame_time_width != 20 && vad_frame_time_width != 30)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (vad_correlate < WEBRTC_VAD_CORRELATED || vad_correlate > WEBRTC_VAD_MIXTURE)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (pre_ns)
    {
        if (ns_agn < WEBRTC_NS_MILD || ns_agn > WEBRTC_NS_HIGHEST)
        {
            return NATIVE_AUDIO_ERR_INVALID_PARAM;
        }
    }

    if (sample_rate != 8000  && sample_rate != 16000 && 
        sample_rate != 32000 && sample_rate != 48000)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    // check_frame_width必须是100ms的整数倍，并且是vad_frame_width的整数倍
    if (check_frame_time_width <= 0 || check_frame_time_width % 100 != 0 || check_frame_time_width % vad_frame_time_width != 0)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    // 滑动窗口必须是100ms的倍数，必须是检测宽度的倍数，必须能被检测步长整除
    if (slide_window_time_width <= 0 || slide_window_time_width % 100 != 0 || slide_window_time_width % vad_frame_time_width != 0 || 
        slide_window_time_width > check_frame_time_width || check_frame_time_width % slide_window_time_width != 0)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (!vad_session)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    session = (yt_vad_session*)calloc(1, sizeof(yt_vad_session));
    if (!session)

    {
        return NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
    }

    session->check_frame_bytes_width = sample_rate / 1000 * check_frame_time_width * 2;
    session->slide_window_bytes_width = sample_rate / 1000 * slide_window_time_width * 2;
    session->check_res_length = check_frame_time_width / vad_frame_time_width;

    session->audio_stream_buffer = (char*)calloc(session->check_frame_bytes_width * 2, sizeof(char));
    session->slide_window_buffer = (char*)calloc(session->slide_window_bytes_width, sizeof(char));
    session->vad_check_res = (int*)calloc(session->check_res_length * 2, sizeof(int));
    if (!(session->audio_stream_buffer) || !(session->slide_window_buffer) || !(session->vad_check_res))
    {
        ret = NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
        goto error;
    }

    if (pre_ns)
    {
        session->ns_opt = NativeWebrtcNsOpts + WEBRTC_NSX_MOLD;
        ret = session->ns_opt->create(sample_rate, 1u, sample_rate / 1000 * slide_window_time_width, ns_agn, &(session->ns_handle));
        if (ret != NATIVE_AUDIO_ERR_SUCCESS)
        {
            goto error;
        }
    }

    ret = WebRtcVad_Create(&(session->vad_handle));
    if (ret)
    {
        ret = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    ret |= WebRtcVad_Init(session->vad_handle);
    ret |= WebRtcVad_set_mode(session->vad_handle, vad_agn);
    if (ret)
    {
        ret = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    session->sample_rate = sample_rate;
    session->vad_agn = vad_agn;
    session->vad_frame_time_width = vad_frame_time_width;
    session->vad_correlate = vad_correlate;
    session->vad_res_length_per_frame = check_frame_time_width / vad_frame_time_width;
    session->vad_res_length_per_window = slide_window_time_width / vad_frame_time_width;
    session->audio_stream_pos = 0;
    session->check_res_pos = 0;
    *vad_session = session;
    return NATIVE_AUDIO_ERR_SUCCESS;

error:
    yt_vad_session_destroy(session);
    return ret;
}

static int yt_vad_handle(VadInst *inst, int sample_rate, int vad_frame_time_width,
                             const void *audio_stream, int audio_len, int *vad_res, int *res_len)
{
    int index = 0;
    int audio_section_num = 0;
    int audio_section_frame_num = 0;
    const int16_t *audio_frame = (const int16_t*)audio_stream;
    int ret = NATIVE_AUDIO_ERR_SUCCESS;

    // 这里的参数判断在有些调用流程中可能重复，但是想在这里保证函数的完备性
    if (!inst || !audio_stream || !vad_res || !res_len || audio_len <= 0)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (sample_rate != 8000  && sample_rate != 16000 && 
        sample_rate != 32000 && sample_rate != 48000)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (vad_frame_time_width != 10 && vad_frame_time_width != 20 && vad_frame_time_width != 30)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    audio_section_frame_num = sample_rate / 1000 * vad_frame_time_width;
    if (audio_len % (audio_section_frame_num * 2) != 0)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    audio_section_num = audio_len / (audio_section_frame_num * 2);
    for (index = 0; index < audio_section_num; ++index)
    {
        ret = WebRtcVad_Process(inst, sample_rate, audio_frame + (index * audio_section_frame_num), audio_section_frame_num);
        if (ret < 0)
        {
            return NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        }
        vad_res[index] = ret;
    }

    *res_len = audio_section_num;
    return NATIVE_AUDIO_ERR_SUCCESS;
}

static int yt_vad_operator(yt_vad_session *vad_session, const void *audio_stream, int audio_len, const int **check_res)
{
    const void *pre_audio = NULL;
    int vad_res_length = 0;
    int ret = NATIVE_AUDIO_ERR_SUCCESS;

    if (!vad_session || !audio_stream || audio_len <= 0 || !check_res)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    if (audio_len != vad_session->slide_window_bytes_width)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    // 前置降噪处理
    if (vad_session->ns_handle)
    {
        ret = vad_session->ns_opt->frame_handle(vad_session->ns_handle, audio_stream, vad_session->slide_window_buffer);
        if (ret != NATIVE_AUDIO_ERR_SUCCESS)
        {
            return ret;
        }
        pre_audio = vad_session->slide_window_buffer;
    }
    else
    {
        pre_audio = audio_stream;
    }

    switch (vad_session->vad_correlate)
    {
        case WEBRTC_VAD_CORRELATED:
        {
            ret = yt_vad_handle(vad_session->vad_handle, vad_session->sample_rate, vad_session->vad_frame_time_width, pre_audio,
                                    vad_session->slide_window_bytes_width, vad_session->vad_check_res + vad_session->check_res_pos, &vad_res_length);

            if (ret == NATIVE_AUDIO_ERR_SUCCESS && vad_res_length == vad_session->vad_res_length_per_window)
            {
                memcpy(vad_session->vad_check_res + vad_session->check_res_pos + vad_session->check_res_length, 
                       vad_session->vad_check_res + vad_session->check_res_pos,
                       vad_session->vad_res_length_per_window * sizeof(vad_session->vad_check_res[0]));
                vad_session->check_res_pos += vad_session->vad_res_length_per_window;
                *check_res = vad_session->vad_check_res + vad_session->check_res_pos;
            }
            else if (ret == NATIVE_AUDIO_ERR_SUCCESS)
            {
                ret = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            }

            if (vad_session->check_res_pos >= vad_session->check_res_length)
            {
                vad_session->check_res_pos = 0;
            }
            return ret;
        }
        case WEBRTC_VAD_UNCORRELATED:
        {
            if (vad_session->vad_handle)
            {
                WebRtcVad_Free(vad_session->vad_handle);
                vad_session->vad_handle = NULL;
            }

            ret = WebRtcVad_Create(&(vad_session->vad_handle));
            if (ret)
            {
                return NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            }

            ret |= WebRtcVad_Init(vad_session->vad_handle);
            ret |= WebRtcVad_set_mode(vad_session->vad_handle, vad_session->vad_agn);
            if (ret)
            {
                WebRtcVad_Free(vad_session->vad_handle);
                vad_session->vad_handle = NULL;
                return NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            }
        }
        case WEBRTC_VAD_MIXTURE:
        {
            memcpy(vad_session->audio_stream_buffer + vad_session->audio_stream_pos,
                   pre_audio, vad_session->slide_window_bytes_width);
            memcpy(vad_session->audio_stream_buffer + vad_session->audio_stream_pos + vad_session->check_frame_bytes_width,
                   pre_audio, vad_session->slide_window_bytes_width);
            vad_session->audio_stream_pos += vad_session->slide_window_bytes_width;

            ret = yt_vad_handle(vad_session->vad_handle, vad_session->sample_rate, vad_session->vad_frame_time_width,
                                    vad_session->audio_stream_buffer + vad_session->audio_stream_pos, 
                                    vad_session->check_frame_bytes_width, vad_session->vad_check_res, &vad_res_length);

            if (ret == NATIVE_AUDIO_ERR_SUCCESS && vad_res_length == vad_session->vad_res_length_per_frame)
            {
                *check_res = vad_session->vad_check_res;
            }
            else if (ret == NATIVE_AUDIO_ERR_SUCCESS)
            {
                ret = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            }

            if (vad_session->audio_stream_pos >= vad_session->check_frame_bytes_width)
            {
                vad_session->audio_stream_pos = 0;
            }
            return ret;
        }
        default:
        {
            return NATIVE_AUDIO_ERR_INVALID_PARAM;
        }
    }
}

yt_vad_session * yt_webrtc_create_vad_session(int vad_agn, int vad_frame_time_width,
                                                    int vad_correlate, int pre_ns, int ns_agn, int sample_rate,
                                                    int check_frame_time_width, int slide_window_time_width)
{
    yt_vad_session * vad_session = NULL;
    int ret = yt_vad_session_create(vad_agn, vad_frame_time_width, vad_correlate, pre_ns, ns_agn,
                                        sample_rate, check_frame_time_width, slide_window_time_width, &vad_session);
    return (ret == NATIVE_AUDIO_ERR_SUCCESS) ? vad_session : NULL;
}

void yt_webrtc_destroy_vad_session(yt_vad_session *session)
{
    yt_vad_session_destroy(session);
}

int yt_webrtc_vad_operator(yt_vad_session *session, void *audio_stream, unsigned int audio_stream_length, int *result)
{
    int index = 0;
    const int *vad_check_result = NULL;
    int ret = NATIVE_AUDIO_ERR_SUCCESS;
    if(!session)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    ret = yt_vad_operator(session, audio_stream, audio_stream_length, &vad_check_result);
    if (ret != NATIVE_AUDIO_ERR_SUCCESS)
    {
        goto error;
    }

    for (index = 0; index < session->vad_res_length_per_frame ; ++index)
    {
        result[index] = vad_check_result[index];
    }
    ret = NATIVE_AUDIO_ERR_SUCCESS;
    
error:
    return ret;
}

int yt_webrtc_vad_res_length(yt_vad_session *session)
{
    return yt_vad_res_length(session);
}
