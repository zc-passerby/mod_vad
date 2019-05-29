/*
 *
 * mod_vad.c VAD code with optional libfvad
 *
 */

#include <switch.h>
#include <sys/time.h>
#include <fvad.h>

#define VAD_PRIVATE "_vad_"
#define VAD_XML_CONFIG "vad.conf"
#define VAD_EVENT_SUBCLASS "vad::detection"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_vad_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load);
SWITCH_MODULE_DEFINITION(mod_vad, mod_vad_load, mod_vad_shutdown, NULL);
SWITCH_STANDARD_APP(vad_start_function);

typedef enum {
	SWITCH_VAD_STATE_NONE,
	SWITCH_VAD_STATE_START_TALKING,
	SWITCH_VAD_STATE_TALKING,
	SWITCH_VAD_STATE_STOP_TALKING,
	SWITCH_VAD_STATE_ERROR
} switch_vad_state_t;


typedef struct {
    switch_core_session_t *session;
    switch_codec_implementation_t *read_impl;
    switch_media_bug_t *read_bug;
    switch_audio_resampler_t *read_resampler;

    int talking;
    int talked;
    int talk_hits;
    int listen_hits;
    int hangover;
    int hangover_len;
    int divisor;
    int thresh;
    int channels;
    int sample_rate;
    int debug;
    int _hangover_len;
    int _thresh;
    int _listen_hits;
    switch_vad_state_t vad_state;
    Fvad *fvad;
} switch_vad_t;

static struct {
    int mode;
    int skip_len;
    int hangover_len;
    int threshold;
    int listen_hits;
    int timeout_len;
    
} globals;

SWITCH_DECLARE(const char *) switch_vad_state2str(switch_vad_state_t state)
{
    switch(state) {
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

    if(!(xml = switch_xml_open_cfg(VAD_XML_CONFIG, &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open XML configuration '%s'\n", VAD_XML_CONFIG);
        return -1;
    }

    if((settings = switch_xml_child(cfg, "settings"))) {
        for(param = switch_xml_child(settings, "param"); param; param = param->next) {
            char *var = (char *)switch_xml_attr_soft(param, "name");
            char *val = (char *)switch_xml_attr_soft(param, "value");
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Found parameter %s=%s\n", var, val);
            if(!strcasecmp(var, "mode")) {
                globals.mode = atoi(val);
            } else if(!strcasecmp(var, "skip_len")) {
                globals.skip_len = atoi(val);
            }
        }
    }

    switch_xml_free(xml);
    return 0;
}

static switch_bool_t switch_vad_init(switch_vad_t *vad)
{
    if(NULL == vad)
        return SWITCH_FALSE;

    vad->_hangover_len = globals.hangover_len;
    vad->_thresh = globals.threshold;
    vad->_listen_hits = globals.listen_hits;
    vad->talking = 0;
    vad->talked = 0;
    vad->talk_hits = 0;
    vad->hangover = 0;
    vad->listen_hits = vad->_listen_hits;
    vad->hangover_len = vad->_hangover_len;
    vad->divisor = vad->sample_rate / 8000;
    vad->thresh = vad->_thresh;
    vad->vad_state = SWITCH_VAD_STATE_NONE;

    if(globals.mode < 0) {
        if(vad->fvad)
        {
            fvad_free(vad->fvad);
            vad->fvad = NULL;
            return SWITCH_FALSE;
        }
    } else if(globals.mode > 3) {
        globals.mode = 3;
    }

    if(NULL == vad->fvad) {
        vad->fvad = fvad_new();
        if(NULL == vad->fvad)
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "libfvad init error\n");
    } else {
        fvad_reset(vad->fvad);
    }

    if(vad->fvad) {
        fvad_set_mode(vad->fvad, globals.mode);
        fvad_set_sample_rate(vad->fvad, vad->sample_rate);
    }

    return SWITCH_TRUE;
}

SWITCH_DECLARE(switch_vad_state_t) switch_vad_process(switch_vad_t *vad, int16_t *data, unsigned int samples)
{
    int energy = 0, j = 0, count = 0;
    int score = 0;

    if(vad->vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
        vad->vad_state = SWITCH_VAD_STATE_NONE;
    } else if (vad->vad_state == SWITCH_VAD_STATE_START_TALKING) {
        vad->vad_state = SWITCH_VAD_STATE_TALKING;
    }

    if(vad->fvad) {
        int ret = fvad_process(vad->fvad, data, samples);
        score = vad->thresh + ret - 1;
    } else {
        for(energy = 0, j = 0, count = 0; count < samples; count++) {
            energy += abs(data[j]);
            j += vad->channels;
        }

        score = (uint32_t)(energy / (samples / vad->divisor));
    }

    if(vad->talking && score < vad->thresh) {
        if(vad->hangover > 0) {
            vad->hangover--;
        } else {
            vad->talking = 0;
            vad->talk_hits = 0;
            vad->hangover = 0;
        }
    } else {
        if(score >= vad->thresh) {
            vad->vad_state = vad->talking ? SWITCH_VAD_STATE_TALKING : SWITCH_VAD_STATE_START_TALKING;
            vad->talking = 1;
            vad->hangover = vad->hangover_len;
        }
    }

    if(vad->talking) {
        vad->talk_hits++;
        if(vad->talk_hits > vad->listen_hits) {
            vad->talked = 1;
            vad->vad_state = SWITCH_VAD_STATE_TALKING;
        }
    } else {
        vad->talk_hits = 0;
    }

    if(vad->talked && !vad->talking) {
        vad->talked = 0;
        vad->vad_state = SWITCH_VAD_STATE_STOP_TALKING;
    }

    if(vad->debug > 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "VAD DEBUG energy: %d state %s\n", score, switch_vad_state2str(vad->vad_state));
    }

    return vad->vad_state;
}

static switch_bool_t fire_vad_event(switch_core_session_t *session, switch_vad_state_t vad_state)
{
    switch_event_t *event = NULL;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Fire VAD event %d\n", vad_state);
    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, VAD_EVENT_SUBCLASS);
    if(event) {
        switch(vad_state) {
            case SWITCH_VAD_STATE_START_TALKING:
                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "start_talking");
                break;
            case SWITCH_VAD_STATE_STOP_TALKING:
                switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "vad_state", "stop_talking");
                break;
            default:
                break;
        }
        switch_channel_event_set_data(channel, event);
        switch_event_fire(&event);
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to fire VAD Complete event %d\n", vad_state);
    }

    return SWITCH_TRUE;
}

static switch_bool_t vad_audio_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    switch_vad_t *vad = (switch_vad_t *)user_data;
    switch_core_session_t *session = vad->session;
    switch_vad_state_t vad_state;
    switch_frame_t *linear_frame;
    uint32_t linear_len = 0;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    if(!switch_channel_media_ready(channel)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Channel codec isn't ready\n");
        return SWITCH_TRUE;
    }

    switch(type) {
        case SWITCH_ABC_TYPE_INIT:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Starting VAD detection for audio stream\n");
            break;
        case SWITCH_ABC_TYPE_CLOSE:
            if(vad->read_resampler) {
                switch_resample_destroy(&vad->read_resampler);
            }

            if(vad->fvad) {
                fvad_free(vad->fvad);
                vad->fvad = NULL;
            }

            switch_core_media_bug_flush(bug);
            switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Stopping VAD detection for audio stream\n");
            break;
        case SWITCH_ABC_TYPE_READ:
        case SWITCH_ABC_TYPE_READ_REPLACE:
            linear_frame = switch_core_media_bug_get_read_replace_frame(bug);
            linear_len = linear_frame->datalen;

            vad_state = switch_vad_process(vad, linear_frame->data, linear_len / 2);
            if(vad_state == SWITCH_VAD_STATE_START_TALKING) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "START TALKING\n");
            } else if(vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "STOP TALKING\n");
            } else if(vad_state == SWITCH_VAD_STATE_TALKING) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "State - TALKING\n");
            }
            break;
        default:
            break;
    }

    return SWITCH_TRUE;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_vad_load)
{
    switch_application_interface_t *app_interface;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    if(load_config()) {
        return SWITCH_STATUS_UNLOAD;
    }

    SWITCH_ADD_APP(app_interface, "vad", "Voice activity detection", "Freeswitch's VAD", vad_start_function, "[start|stop]", SAF_NONE);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " vad_load successful...\n");

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
    switch_vad_t *s_vad = NULL;
    switch_codec_implementation_t imp = { 0 };
    int flags = 0;

    if(!zstr(data)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "VAD input parameter %s\n", data);
    }

    if((s_vad = (switch_vad_t *)switch_channel_get_private(channel, VAD_PRIVATE))) {
        if(!zstr(data) && !strcasecmp(data, "stop")) {
            switch_channel_set_private(channel, VAD_PRIVATE, NULL);
            if(s_vad->read_bug) {
                switch_core_media_bug_remove(session, &s_vad->read_bug);
                s_vad->read_bug = NULL;
                switch_core_session_reset(session, SWITCH_TRUE, SWITCH_TRUE);
            }
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Stopped VAD detection\n");
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Cannot run vad detection 2 times on the same session!\n");
        }
        return;
    }

    s_vad = switch_core_session_alloc(session, sizeof(*s_vad));
    switch_assert(s_vad);
    memset(s_vad, 0, sizeof(*s_vad));
    s_vad->session = session;

    switch_core_session_raw_read(session);
    switch_core_session_get_read_impl(session, &imp);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Read imp %u %u.\n", imp.samples_per_second, imp.number_of_channels);
    s_vad->sample_rate = imp.samples_per_second ? imp.samples_per_second : 8000;
    s_vad->channels = imp.number_of_channels;

    // just for fvad set!
    switch_vad_init(s_vad);

    flags = SMBF_READ_REPLACE | SMBF_ANSWER_REQ;
    status = switch_core_media_bug_add(session, "vad_read", NULL, vad_audio_callback, s_vad, 0, flags, &s_vad->read_bug);

    if(status != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Failed to attach vad to media stream!\n");
        return;
    }

    switch_channel_set_private(channel, VAD_PRIVATE, s_vad);
}
