#include "yt_webrtc_media_comm.h"
#include "yt_webrtc_ns.h"
#include "noise_suppression.h"
#include "noise_suppression_x.h"
#include "signal_processing_library.h"

#include <string.h>
#include <stdlib.h>

typedef struct AudioBuffer
{
    unsigned int samples_per_channel;
    // 是否分高低频
    bool is_split;
    WebRtc_Word16 *data;
    WebRtc_Word16 low_pass_data[160];
    WebRtc_Word16 high_pass_data[160];

    WebRtc_Word32 analysis_filter_state1[6];
    WebRtc_Word32 analysis_filter_state2[6];
    WebRtc_Word32 synthesis_filter_state1[6];
    WebRtc_Word32 synthesis_filter_state2[6];
} AudioBuffer;

static int AudioBuffer_Init(AudioBuffer *ab, unsigned int sample_rate)
{
    if (!ab || !sample_rate)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    memset(ab, 0, sizeof(AudioBuffer));
    if (sample_rate == 32000)
    {
        ab->is_split = true;
        ab->samples_per_channel = 160;
    }
    else
    {
        ab->is_split = false;
        ab->samples_per_channel = sample_rate / 100;
    }
    return NATIVE_AUDIO_ERR_SUCCESS;
}

static int AudioBuffer_SetData(AudioBuffer *ab, WebRtc_Word16 *pdata)
{
    if (!ab || !pdata)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    ab->data = pdata;
    if (ab->is_split)
    {
        WebRtcSpl_AnalysisQMF(ab->data,                      /* input data */
                              ab->low_pass_data,             /* pointer to low pass data storage*/
                              ab->high_pass_data,            /* pointer to high pass data storage*/
                              ab->analysis_filter_state1,
                              ab->analysis_filter_state2);
    }
    return NATIVE_AUDIO_ERR_SUCCESS;
}

static WebRtc_Word16* AudioBuffer_GetData(AudioBuffer *ab)
{
    if (ab)
    {
        if (ab->is_split)
        {
            WebRtcSpl_SynthesisQMF(ab->low_pass_data,
                                   ab->high_pass_data,
                                   ab->data,
                                   ab->synthesis_filter_state1,
                                   ab->synthesis_filter_state2);
        }
        return ab->data;
    }
    return NULL;
}

static WebRtc_Word16* AudioBuffer_GetLowPassData(AudioBuffer *ab)
{
    if (ab)
    {
        return (ab->is_split) ? ab->low_pass_data : ab->data;
    }
    return NULL;
}

static WebRtc_Word16* AudioBuffer_GetHighPassData(AudioBuffer *ab)
{
    if (ab)
    {
        return (ab->is_split) ? ab->high_pass_data : ab->data;
    }
    return NULL;
}

static unsigned int AudioBuffer_SamplesPerChannel(AudioBuffer *ab)
{
    if (ab)
    {
        return ab->samples_per_channel;
    }
    return 0;
}

// fileter high rate
static const WebRtc_Word16 kFilterCoefficients8kHz[5] = {3798, -7596, 3798, 7807, -3733};
static const WebRtc_Word16 kFilterCoefficients[5] = {4012, -8024, 4012, 8002, -3913};

typedef struct
{
    WebRtc_Word16 x[2];
    WebRtc_Word16 y[4];
    const WebRtc_Word16* ba;
} HighPassFilterState;

static int HighPassFilter_Initialize(HighPassFilterState *hpf, unsigned int sample_rate)
{
    if (hpf && sample_rate)
    {
        hpf->ba = (sample_rate == 8000) ? kFilterCoefficients8kHz : kFilterCoefficients;
        WebRtcSpl_MemSetW16(hpf->x, 0, 2);
        WebRtcSpl_MemSetW16(hpf->y, 0, 4);
        return NATIVE_AUDIO_ERR_SUCCESS;
    }
    return NATIVE_AUDIO_ERR_INVALID_PARAM;
}

static int HighPassFilter_Process(HighPassFilterState *hpf, WebRtc_Word16 *data, unsigned int length)
{
    unsigned int i;
    WebRtc_Word32 tmp_int32 = 0;
    WebRtc_Word16* y = hpf->y;
    WebRtc_Word16* x = hpf->x;
    const WebRtc_Word16* ba = hpf->ba;
    
    if (!hpf || !data || !length)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    for (i = 0; i < length; i++) 
    {
        //  y[i] = b[0] * x[i] + b[1] * x[i-1] + b[2] * x[i-2]
        //         + -a[1] * y[i-1] + -a[2] * y[i-2];

        tmp_int32 = WEBRTC_SPL_MUL_16_16(y[1], ba[3]); // -a[1] * y[i-1] (low part)
        tmp_int32 += WEBRTC_SPL_MUL_16_16(y[3], ba[4]); // -a[2] * y[i-2] (low part)
        tmp_int32 = (tmp_int32 >> 15);
        tmp_int32 += WEBRTC_SPL_MUL_16_16(y[0], ba[3]); // -a[1] * y[i-1] (high part)
        tmp_int32 += WEBRTC_SPL_MUL_16_16(y[2], ba[4]); // -a[2] * y[i-2] (high part)
        tmp_int32 = (tmp_int32 << 1);

        tmp_int32 += WEBRTC_SPL_MUL_16_16(data[i], ba[0]); // b[0]*x[0]
        tmp_int32 += WEBRTC_SPL_MUL_16_16(x[0], ba[1]);    // b[1]*x[i-1]
        tmp_int32 += WEBRTC_SPL_MUL_16_16(x[1], ba[2]);    // b[2]*x[i-2]

        // Update state (input part)
        x[1] = x[0];
        x[0] = data[i];

        // Update state (filtered part)
        y[2] = y[0];
        y[3] = y[1];
        y[0] = (WebRtc_Word16)(tmp_int32 >> 13);
        y[1] = (WebRtc_Word16)((tmp_int32 - WEBRTC_SPL_LSHIFT_W32((WebRtc_Word32)(y[0]), 13)) << 2);

        // Rounding in Q12, i.e. add 2^11
        tmp_int32 += 2048;

        // Saturate (to 2^27) so that the HP filtered signal does not overflow
        tmp_int32 = WEBRTC_SPL_SAT((WebRtc_Word32)(134217727), tmp_int32, (WebRtc_Word32)(-134217728));

        // Convert back to Q0 and use rounding
        data[i] = (WebRtc_Word16)WEBRTC_SPL_RSHIFT_W32(tmp_int32, 12);
    }
    return NATIVE_AUDIO_ERR_SUCCESS;
}

typedef struct yunfan_webrtc_ns
{
    union
    {
        NsHandle            *NS_inst;
        NsxHandle           *NSX_inst;
    };
    unsigned int            sample_rate;
    unsigned int            samples_per_frame;
    unsigned int            samples_per_10ms_frame;
    // audio media buffer
    AudioBuffer             capture_audio_buffer;
    // filter high rate
    HighPassFilterState     hpf;
    short                   *tmp_frame;
} yunfan_webrtc_ns;

int yunfan_webrtc_ns_destroy(void *hnd)
{
    if (hnd)
    {
        yunfan_webrtc_ns *webrtc_ns = (yunfan_webrtc_ns*)hnd;
        if (webrtc_ns->NS_inst)
        {
            WebRtcNs_Free(webrtc_ns->NS_inst);
            webrtc_ns->NS_inst = NULL;
        }
        if (webrtc_ns->tmp_frame)
        {
            free(webrtc_ns->tmp_frame);
            webrtc_ns->tmp_frame = NULL;
        }
        free(webrtc_ns);
        return NATIVE_AUDIO_ERR_SUCCESS;
    }
    return NATIVE_AUDIO_ERR_INVALID_PARAM;
}

int yunfan_webrtc_ns_create(unsigned int sample_rate, 
                            unsigned int channel_count,
                            unsigned int sample_per_frame,
                            int policy,
                            void **pp_hnd)
{
    yunfan_webrtc_ns *webrtc_ns = NULL;
    int status = NATIVE_AUDIO_ERR_SUCCESS;

    if (// 采样率必须是8000，16000，32000
        (sample_rate != 8000 && sample_rate != 16000 && sample_rate != 32000) ||
        // 必须是单声道
        channel_count != 1 || 
        // 每一帧必须包含n*10ms的音频数据
        sample_per_frame < sample_rate / 100 || sample_per_frame % (sample_rate / 100) > 0 || 
        policy < WEBRTC_NS_MILD || policy >= WEBRTC_NS_INVALID || !pp_hnd)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    webrtc_ns = (yunfan_webrtc_ns*)calloc(1, sizeof(yunfan_webrtc_ns));
    if (!webrtc_ns)
    {
        return NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
    }

    webrtc_ns->tmp_frame = (short*)calloc(sample_per_frame * channel_count, sizeof(short));
    if (!(webrtc_ns->tmp_frame))
    {
        status = NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
        goto error;
    }

    status = HighPassFilter_Initialize(&(webrtc_ns->hpf), sample_rate);
    if (status != 0)
    {
        goto error;
    }

    status = WebRtcNs_Create(&(webrtc_ns->NS_inst));
    if(status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
    	goto error;
    }

    status = WebRtcNs_Init(webrtc_ns->NS_inst, sample_rate);
    if(status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    status = WebRtcNs_set_policy(webrtc_ns->NS_inst, policy);
    if (status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    webrtc_ns->sample_rate = sample_rate;
    webrtc_ns->samples_per_frame = sample_per_frame;
    webrtc_ns->samples_per_10ms_frame = sample_rate / 100;
    AudioBuffer_Init(&(webrtc_ns->capture_audio_buffer), sample_rate);
    *pp_hnd = (void*)webrtc_ns;
    return NATIVE_AUDIO_ERR_SUCCESS;
    
error:
    yunfan_webrtc_ns_destroy(webrtc_ns);
    return status;
}

int yunfan_webrtc_ns_frame_handle(void *hnd, const char *src_frame, char *dst_frame)
{
    yunfan_webrtc_ns *webrtc_ns = (yunfan_webrtc_ns*)hnd;
    const short *src_frm = (const short*)src_frame;
    short *dst_frm = (short*)dst_frame;
    int status = NATIVE_AUDIO_ERR_SUCCESS;
    unsigned int index = 0;

    if (!hnd || !src_frame || !dst_frame)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    media_copy_samples(webrtc_ns->tmp_frame, src_frm, webrtc_ns->samples_per_frame);
    // 以10ms为单位，循环处理音频数据
    for (index = 0; index < webrtc_ns->samples_per_frame; index += webrtc_ns->samples_per_10ms_frame)
    {
        AudioBuffer_SetData(&(webrtc_ns->capture_audio_buffer), (WebRtc_Word16*)(webrtc_ns->tmp_frame + index));

#if YUNFAN_WEBRTC_NS_USE_HIGH_PASS_FILER
        /* Apply high pass filer */
        HighPassFilter_Process(&(webrtc_ns->hpf),
                               AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                               AudioBuffer_SamplesPerChannel(&(webrtc_ns->capture_audio_buffer)));
#endif

        /* Noise suppression */
        status = WebRtcNs_Process(webrtc_ns->NS_inst,
                                  AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetHighPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetHighPassData(&(webrtc_ns->capture_audio_buffer)));
        if (status != 0)
        {
            status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            return status;
        }
        AudioBuffer_GetData(&(webrtc_ns->capture_audio_buffer));
    }
    media_copy_samples(dst_frm, webrtc_ns->tmp_frame, webrtc_ns->samples_per_frame);
    return status;
}

int yunfan_webrtc_nsx_destroy(void *hnd)
{
    if (hnd)
    {
        yunfan_webrtc_ns *webrtc_ns = (yunfan_webrtc_ns*)hnd;
        if (webrtc_ns->NSX_inst)
        {
            WebRtcNsx_Free(webrtc_ns->NSX_inst);
            webrtc_ns->NSX_inst = NULL;
        }
        if (webrtc_ns->tmp_frame)
        {
            free(webrtc_ns->tmp_frame);
            webrtc_ns->tmp_frame = NULL;
        }
        free(webrtc_ns);
        return NATIVE_AUDIO_ERR_SUCCESS;
    }
    return NATIVE_AUDIO_ERR_INVALID_PARAM;
}

int yunfan_webrtc_nsx_create(unsigned int sample_rate, 
                             unsigned int channel_count,
                             unsigned int sample_per_frame,
                             int policy,
                             void **pp_hnd)
{
    yunfan_webrtc_ns *webrtc_ns = NULL;
    int status = NATIVE_AUDIO_ERR_SUCCESS;

    if (// 采样率必须是8000，16000，32000
        (sample_rate != 8000 && sample_rate != 16000 && sample_rate != 32000) ||
        // 必须是单声道
        channel_count != 1 || 
        // 每一帧必须包含n*10ms的音频数据
        sample_per_frame < sample_rate / 100 || sample_per_frame % (sample_rate / 100) > 0 || 
        policy < WEBRTC_NS_MILD || policy >= WEBRTC_NS_INVALID || !pp_hnd)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    webrtc_ns = (yunfan_webrtc_ns*)calloc(1, sizeof(yunfan_webrtc_ns));
    if (!webrtc_ns)
    {
        return NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
    }

    webrtc_ns->tmp_frame = (short*)calloc(sample_per_frame * channel_count, sizeof(short));
    if (!(webrtc_ns->tmp_frame))
    {
        status = NATIVE_AUDIO_ERR_NOENOUGH_MEMORY;
        goto error;
    }

    status = HighPassFilter_Initialize(&(webrtc_ns->hpf), sample_rate);
    if (status != 0)
    {
        goto error;
    }

    status = WebRtcNsx_Create(&(webrtc_ns->NSX_inst));
    if(status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    status = WebRtcNsx_Init(webrtc_ns->NSX_inst, sample_rate);
    if(status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    status = WebRtcNsx_set_policy(webrtc_ns->NSX_inst, policy);
    if (status != 0)
    {
        status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
        goto error;
    }

    webrtc_ns->sample_rate = sample_rate;
    webrtc_ns->samples_per_frame = sample_per_frame;
    webrtc_ns->samples_per_10ms_frame = sample_rate / 100;
    AudioBuffer_Init(&(webrtc_ns->capture_audio_buffer), sample_rate);
    *pp_hnd = (void*)webrtc_ns;
    return NATIVE_AUDIO_ERR_SUCCESS;
    
error:
    yunfan_webrtc_nsx_destroy(webrtc_ns);
    return status;
}

int yunfan_webrtc_nsx_frame_handle(void *hnd, const char *src_frame, char *dst_frame)
{
    yunfan_webrtc_ns *webrtc_ns = (yunfan_webrtc_ns*)hnd;
    const short *src_frm = (const short*)src_frame;
    short *dst_frm = (short*)dst_frame;
    int status = NATIVE_AUDIO_ERR_SUCCESS;
    unsigned int index = 0;

    if (!hnd || !src_frame || !dst_frame)
    {
        return NATIVE_AUDIO_ERR_INVALID_PARAM;
    }

    media_copy_samples(webrtc_ns->tmp_frame, src_frm, webrtc_ns->samples_per_frame);
    // 以10ms为单位，循环处理音频数据
    for (index = 0; index < webrtc_ns->samples_per_frame; index += webrtc_ns->samples_per_10ms_frame)
    {
        AudioBuffer_SetData(&(webrtc_ns->capture_audio_buffer), (WebRtc_Word16*)(webrtc_ns->tmp_frame + index));

#if YUNFAN_WEBRTC_NS_USE_HIGH_PASS_FILER
                /* Apply high pass filer */
                HighPassFilter_Process(&(webrtc_ns->hpf),
                                       AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                                       AudioBuffer_SamplesPerChannel(&(webrtc_ns->capture_audio_buffer)));
#endif

        /* Noise suppression */
        status = WebRtcNsx_Process(webrtc_ns->NSX_inst,
                                  AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetHighPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetLowPassData(&(webrtc_ns->capture_audio_buffer)),
                                  AudioBuffer_GetHighPassData(&(webrtc_ns->capture_audio_buffer)));
        if (status != 0)
        {
            status = NATIVE_AUDIO_ERR_WEBRTCAUDIO_ABNORMAL;
            return status;
        }
        AudioBuffer_GetData(&(webrtc_ns->capture_audio_buffer));
    }
    media_copy_samples(dst_frm, webrtc_ns->tmp_frame, webrtc_ns->samples_per_frame);
    return status;
}

