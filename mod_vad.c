/*
 *
 * mod_vad.c VAD code with optional libfvad
 *
 */

#include <switch.h>
#include <sys/time.h>
#include <yt_webrtc_media.h>

#define VAD_PRIVATE "_vad_"
#define VAD_XML_CONFIG "vad.conf"
#define VAD_BUG_NAME_READ "vad_read"
#define VAD_EVENT_SUBCLASS "vad::detection"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load);
SWITCH_MODULE_DEFINITION(mod_vad, mod_vad_load, mod_vad_shutdown, NULL);
SWITCH_STANDARD_APP(vad_start_function);

typedef enum
{
    SWITCH_VAD_STATE_NONE,
    SWITCH_VAD_STATE_START_TALKING,
    SWITCH_VAD_STATE_TALKING,
    SWITCH_VAD_STATE_STOP_TALKING,
    SWITCH_VAD_STATE_ERROR
} switch_vad_state_t;

typedef struct
{
    int vad_agn;                 // vad检测模式
    int vad_frame_time_width;    // vad检测时长
    int vad_correlate;           // handle是否复用
    int ns_agn;                  // ns降噪模式
    int check_frame_time_width;  // 检测宽度
    int slide_window_time_width; // 滑动窗口
    int start_talking_ratio;     // 开始说话时的比例
    int stop_talking_ratio;      // 结束说话时的比例

    int talking;
    int channels;
    int sample_rate; // 采样率

    char *media_buffer;
    int media_buffer_len;
    int media_send_len;

    yt_vad_session_t *vad_session;
} switch_vad_t;

typedef struct
{
    switch_core_session_t *session;
    switch_media_bug_t *read_bug;
    switch_audio_resampler_t *read_resampler;

    switch_vad_t *s_vad;
} vad_session_t;

static struct
{
    int vad_agn;
    int vad_frame_time_width;
    int vad_correlate;
    int ns_agn;
    int check_frame_time_width;
    int slide_window_time_width;
    int start_talking_ratio;
    int stop_talking_ratio;
} globals;

const char *switch_vad_state2str(switch_vad_state_t state)
{
    switch (state)
    {
    case SWITCH_VAD_STATE_NONE:
        return "none";
    case SWITCH_VAD_STATE_START_TALKING:
        return "start_talking";
    case SWITCH_VAD_STATE_TALKING:
        return "talking";
    case SWITCH_VAD_STATE_STOP_TALKING:
        return "stop_talking";
    default:
        return "error";
    }
}

static int load_config(void)
{
    switch_xml_t cfg, xml, settings, param;
    if (!(xml = switch_xml_open_cfg(VAD_XML_CONFIG, &cfg, NULL)))
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open XML configuration '%s'\n", VAD_XML_CONFIG);
        return -1;
    }

    if ((settings = switch_xml_child(cfg, "settings")))
    {
        for (param = switch_xml_child(settings, "param"); param; param = param->next)
        {
            char *var = (char *)switch_xml_attr_soft(param, "name");
            char *val = (char *)switch_xml_attr_soft(param, "value");
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Found parameter %s=%s\n", var, val);
            if (!strcasecmp(var, "vad_agn"))
            {
                globals.vad_agn = atoi(val);
            }
            else if (!strcasecmp(var, "vad_frame_time_width"))
            {
                globals.vad_frame_time_width = atoi(val);
            }
            else if (!strcasecmp(var, "vad_correlate"))
            {
                globals.vad_correlate = atoi(val);
            }
            else if (!strcasecmp(var, "ns_agn"))
            {
                globals.ns_agn = atoi(val);
            }
            else if (!strcasecmp(var, "check_frame_time_width"))
            {
                globals.check_frame_time_width = atoi(val);
            }
            else if (!strcasecmp(var, "slide_window_time_width"))
            {
                globals.slide_window_time_width = atoi(val);
            }
            else if (!strcasecmp(var, "start_talking_ratio"))
            {
                globals.start_talking_ratio = atoi(val);
            }
            else if (!strcasecmp(var, "stop_talking_ratio"))
            {
                globals.stop_talking_ratio = atoi(val);
            }
        }
    }

    switch_xml_free(xml);
    return 0;
}

void switch_vad_reset(switch_vad_t *vad)
{
    vad->talking = 0;
    vad->media_buffer_len = 0;
    vad->media_send_len = 0;

    return;
}

switch_vad_t *switch_vad_init(int sample_rate, int channels)
{
    switch_vad_t *vad = malloc(sizeof(switch_vad_t));
    if (!vad)
        return NULL;
    memset(vad, 0, sizeof(*vad));

    vad->sample_rate = sample_rate ? sample_rate : 8000;
    vad->vad_agn = 3;
    vad->vad_frame_time_width = 10;
    vad->vad_correlate = 2;
    vad->ns_agn = 3;
    vad->check_frame_time_width = 600;
    vad->slide_window_time_width = 200;
    vad->channels = channels;
    vad->start_talking_ratio = 70;
    vad->stop_talking_ratio = 30;

    switch_vad_reset(vad);

    return vad;
}

int switch_vad_create(switch_vad_t *vad)
{
    vad->media_send_len = vad->sample_rate / 1000 * vad->slide_window_time_width * 2;
    vad->media_buffer = (char *)calloc(vad->media_send_len, sizeof(char));

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "about to create vad_session : vad_agn=%d, vad_frame_time_width=%d, vad_correlate=%d, ns_agn=%d, "
                                                           "sample_rate=%d, check_frame_time_width=%d, slide_window_time_width=%d\n",
                      vad->vad_agn, vad->vad_frame_time_width, vad->vad_correlate,
                      vad->ns_agn, vad->sample_rate, vad->check_frame_time_width, vad->slide_window_time_width);
    vad->vad_session = yt_webrtc_create_vad_session(vad->vad_agn, vad->vad_frame_time_width, vad->vad_correlate, 1, vad->ns_agn,
                                                    vad->sample_rate, vad->check_frame_time_width, vad->slide_window_time_width);
    return 0;
}

void switch_vad_set_param(switch_vad_t *vad, const char *key, int val)
{
    if (!key)
        return;

    if (!strcmp(key, "vad_agn"))
        vad->vad_agn = val;
    else if (!strcmp(key, "vad_frame_time_width"))
        vad->vad_frame_time_width = val;
    else if (!strcmp(key, "vad_correlate"))
        vad->vad_correlate = val;
    else if (!strcmp(key, "ns_agn"))
        vad->ns_agn = val;
    else if (!strcmp(key, "check_frame_time_width"))
        vad->check_frame_time_width = val;
    else if (!strcmp(key, "slide_window_time_width"))
        vad->slide_window_time_width = val;
    else if (!strcmp(key, "start_talking_ratio"))
        vad->start_talking_ratio = val;
    else if (!strcmp(key, "stop_talking_ratio"))
        vad->stop_talking_ratio = val;
}

switch_vad_state_t switch_vad_process(switch_vad_t *vad, int16_t *data, unsigned int samples)
{
    switch_vad_state_t vad_state = SWITCH_VAD_STATE_NONE;

    if (vad->vad_session)
    {
        int j = 0, speakCount = 0;
        int vad_res_len = yt_webrtc_vad_res_length(vad->vad_session);
        int result[vad_res_len];

        memcpy(vad->media_buffer + vad->media_buffer_len, data, samples);
        vad->media_buffer_len += samples;
        if (vad->media_buffer_len != vad->media_send_len)
        {
            if (vad->media_buffer_len > vad->media_send_len)
                vad->media_buffer_len = 0;
            return vad_state;
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "going to vad operator");
        yt_webrtc_vad_operator(vad->vad_session, vad->media_buffer, vad->media_buffer_len, result);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "samples:%d, mediaLen:%d, target:%d\n",
                          vad->sample_rate, vad->media_buffer_len, vad->sample_rate / 1000 * vad->slide_window_time_width * 2);
        vad->media_buffer_len = 0;
        for (j = 0; j < vad_res_len; j++)
        {
            if (result[j])
                speakCount++;
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "voice detected in %d of %d frames (%.2f%%)\n",
                          speakCount, vad_res_len, vad_res_len ? 100.0 * ((double)speakCount / vad_res_len) : 0.0);

        if (vad->talking)
        {
            if ((100 * speakCount / vad_res_len) <= vad->stop_talking_ratio)
            {
                vad_state = SWITCH_VAD_STATE_STOP_TALKING;
                vad->talking = 0;
            }
            else
                vad_state = SWITCH_VAD_STATE_TALKING;
        }
        else if ((100 * speakCount / vad_res_len) > vad->start_talking_ratio)
        {
            vad_state = SWITCH_VAD_STATE_START_TALKING;
            vad->talking = 1;
        }
    }
    else
    {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "vad->vad_session is NULL\n");
    }

    return vad_state;
}

void switch_vad_destroy(switch_vad_t **vad)
{
    if (*vad)
    {
        if ((*vad)->vad_session)
            yt_webrtc_destroy_vad_session((*vad)->vad_session);

        if ((*vad)->media_buffer)
        {
            free((*vad)->media_buffer);
            (*vad)->media_buffer = NULL;
        }

        free(*vad);
        *vad = NULL;
    }
}

static switch_bool_t fire_vad_event(switch_core_session_t *session, switch_vad_state_t vad_state)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Fire VAD event %d\n", vad_state);
    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, VAD_EVENT_SUBCLASS);
    if (event)
    {
        switch (vad_state)
        {
        case SWITCH_VAD_STATE_START_TALKING:
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "start_talk");
            break;
        case SWITCH_VAD_STATE_STOP_TALKING:
            switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "stop_talk");
            break;
        default:
            break;
        }
        switch_channel_event_set_data(channel, event);
        switch_event_fire(&event);
    }
    else
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to fire VAD Complete event %d\n", vad_state);
    }

    return SWITCH_TRUE;
}

static switch_bool_t vad_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    vad_session_t *vad = (vad_session_t *)user_data;
    switch_core_session_t *session = vad->session;
    switch_vad_state_t vad_state;
    switch_frame_t *linear_frame;
    uint32_t linear_len = 0;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (!switch_channel_media_ready(channel))
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel codec isn't ready\n");
        return SWITCH_TRUE;
    }

    switch (type)
    {
    case SWITCH_ABC_TYPE_INIT:
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Starting VAD detection for audio stream\n");
        break;
    case SWITCH_ABC_TYPE_CLOSE:
        if (vad->read_resampler)
            switch_resample_destroy(&vad->read_resampler);
        if (vad->s_vad)
            switch_vad_destroy(&vad->s_vad);
        switch_core_media_bug_flush(bug);
        switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Stopping VAD detection for audio stream\n");
        break;
    case SWITCH_ABC_TYPE_READ:
    case SWITCH_ABC_TYPE_READ_REPLACE:
        linear_frame = switch_core_media_bug_get_read_replace_frame(bug);
        linear_len = linear_frame->datalen;

        vad_state = switch_vad_process(vad->s_vad, linear_frame->data, linear_len / 2);
        if (vad_state == SWITCH_VAD_STATE_START_TALKING)
        {
            fire_vad_event(session, vad_state);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "START TALKING\n");
        }
        else if (vad_state == SWITCH_VAD_STATE_STOP_TALKING)
        {
            fire_vad_event(session, vad_state);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "STOP TALKING\n");
        }
        else if (vad_state == SWITCH_VAD_STATE_TALKING)
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "State - TALKING\n");
        break;
    default:
        break;
    }
}

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load)
{
    switch_application_interface_t *app_interface;

    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    if (load_config())
    {
        return SWITCH_STATUS_UNLOAD;
    }

    SWITCH_ADD_APP(
        app_interface,
        "vad",
        "Voice activity detection",
        "Freeswitch's VAD",
        vad_start_function,
        "[start|stop]",
        SAF_NONE);

    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown)
{
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(vad_start_function)
{
    switch_status_t status;
    switch_channel_t *channel = switch_core_session_get_channel(session);
    vad_session_t *vad_session = NULL;
    switch_media_bug_t *bug = NULL;
    switch_vad_t *s_vad;
    switch_codec_implementation_t imp = {0};
    int flags = 0;

    if (!zstr(data))
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "VAD input parameter %s\n", data);
    }

    if ((vad_session = (vad_session_t *)switch_channel_get_private(channel, VAD_PRIVATE)))
    {
        if (!zstr(data) && !strcasecmp(data, "stop"))
        {
            switch_channel_set_private(channel, VAD_PRIVATE, NULL);
            if (vad_session->read_bug)
            {
                switch_core_media_bug_remove(session, &vad_session->read_bug);
                vad_session->read_bug = NULL;
                switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Stopped VAD detection\n");
        }
        else
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run vad detection 2 times on the same session!\n");
        }
        return;
    }

    vad_session = switch_core_session_alloc(session, sizeof(*vad_session));
    switch_assert(vad_session);
    memset(vad_session, 0, sizeof(*vad_session));
    vad_session->session = session;

    switch_core_session_raw_read(session);
    switch_core_session_get_read_impl(session, &imp);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Read imp %u %u.\n", imp.samples_per_second, imp.number_of_channels);

    s_vad = switch_vad_init(imp.samples_per_second, imp.number_of_channels);
    switch_assert(s_vad);
    switch_vad_set_param(s_vad, "vad_agn", globals.vad_agn);
    switch_vad_set_param(s_vad, "vad_frame_time_width", globals.vad_frame_time_width);
    switch_vad_set_param(s_vad, "vad_correlate", globals.vad_correlate);
    switch_vad_set_param(s_vad, "ns_agn", globals.ns_agn);
    switch_vad_set_param(s_vad, "check_frame_time_width", globals.check_frame_time_width);
    switch_vad_set_param(s_vad, "slide_window_time_width", globals.slide_window_time_width);
    switch_vad_set_param(s_vad, "start_talking_ratio", globals.start_talking_ratio);
    switch_vad_set_param(s_vad, "stop_talking_ratio", globals.stop_talking_ratio);
    switch_vad_create(s_vad);
    vad_session->s_vad = s_vad;

    flags = SMBF_READ_REPLACE | SMBF_ANSWER_REQ;
    status = switch_core_media_bug_add(session, VAD_BUG_NAME_READ, NULL, vad_audio_callback, vad_session, 0, flags, &bug);

    if (SWITCH_STATUS_SUCCESS != status)
    {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to attach vad to media stream!\n");
        return;
    }

    vad_session->read_bug = bug;
    bug = NULL;
    switch_channel_set_private(channel, VAD_PRIVATE, vad_session);
}