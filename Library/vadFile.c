#include "readconfig.h"

#include <yt_webrtc_media.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sndfile.h>

const char *szConfigFile = "/var/vad/vadFile.cfg";
const char *szRunningInfo =
        "\ncurrent config info:\n"
        "\tvad_agn:%d\n"
        "\tvad_frame_time_width:%d\n"
        "\tvad_correlate:%d\n"
        "\tns_agn:%d\n"
        "\tcheck_frame_time_width:%d\n"
        "\tslide_window_time_width:%d\n"
        "\tstart_talking_ratio:%d\n"
        "\tstop_talking_ratio:%d\n"
        "current file info:\n"
        "\tchannels:%d\n\tsample_rate:%d\n";

static bool gbTalking = false;
static int gnSequence = 0;

static struct {
    int vad_agn;                 // vad检测模式
    int vad_frame_time_width;    // vad检测时长
    int vad_correlate;           // handle是否复用
    int ns_agn;                  // ns降噪模式
    int check_frame_time_width;  // 检测宽度
    int slide_window_time_width; // 滑动窗口
    int start_talking_ratio;     // 开始说话时的比例
    int stop_talking_ratio;      // 结束说话时的比例
} settings;

bool readConfig() {
    struct ast_config *pConfig;
    struct ast_variable *pVariable;

    pConfig = ast_config_load(szConfigFile);
    if (!pConfig) {
        printf("Unable to open configFile:%s\n", szConfigFile);
        return false;
    }

    pVariable = ast_variable_browse(pConfig, "general");
    while (pVariable) {
        if (!strcasecmp(pVariable->name, "vad_agn") && pVariable->value)
            settings.vad_agn = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "vad_frame_time_width") && pVariable->value)
            settings.vad_frame_time_width = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "vad_correlate") && pVariable->value)
            settings.vad_correlate = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "ns_agn") && pVariable->value)
            settings.ns_agn = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "check_frame_time_width") && pVariable->value)
            settings.check_frame_time_width = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "slide_window_time_width") && pVariable->value)
            settings.slide_window_time_width = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "start_talking_ratio") && pVariable->value)
            settings.start_talking_ratio = atoi(pVariable->value);
        else if (!strcasecmp(pVariable->name, "stop_talking_ratio") && pVariable->value)
            settings.stop_talking_ratio = atoi(pVariable->value);

        pVariable = pVariable->next;
    }
    ast_config_destroy(pConfig);

    return true;
}

static bool process_media(SNDFILE *infile, SF_INFO stInInfo, yt_vad_session *vad_session, size_t framelen) {
    bool success = false;
    double *buf0 = NULL;
    int16_t *buf1 = NULL;
    int vadres, prev = -1;
    long frames[2] = {0, 0};
    long segments[2] = {0, 0};
    SNDFILE *pOutFile = NULL;
//    SF_INFO stOutInfo = {0};
    char szOutFilePath[255];

    if (framelen > SIZE_MAX / sizeof(double) || !(buf0 = malloc(framelen * sizeof *buf0)) ||
        !(buf1 = malloc(framelen * sizeof *buf1))) {
        fprintf(stderr, "failed to allocate buffers\n");
        goto end;
    }

    while (sf_read_double(infile, buf0, framelen) == (sf_count_t) framelen) {
        for (size_t i = 0; i < framelen; i++)
            buf1[i] = buf0[i] * INT16_MAX;

        int talking_num = 0;
        int vad_res_len = yt_webrtc_vad_res_length(vad_session);
        int result[vad_res_len];
        int ret = yt_webrtc_vad_operator(vad_session, buf1, framelen, result);
        if (ret < 0) {
            fprintf(stderr, "VAD processing failed\n");
            goto end;
        }
        for (int j = 0; j < vad_res_len; ++j) {
            result[j] = !!result[j];
            vadres = result[j];
            frames[vadres]++;
            if (vadres) talking_num++;
            if (prev != vadres) segments[vadres]++;
            prev = vadres;
        }
        printf("voice detected in %ld of %ld frames (%.2f%%)\n", talking_num, vad_res_len,
               vad_res_len ? 100.0 * ((double) talking_num / vad_res_len) : 0.0);

        int
        ratio = 100.0 * ((double) talking_num / vad_res_len);
        if (gbTalking) {
            if (ratio <= settings.stop_talking_ratio) {
                gbTalking = false;
                if(pOutFile)
                    sf_close(pOutFile);
                pOutFile = NULL;
            } else {
                if(pOutFile)
                    sf_write_double(pOutFile, buf0, framelen);
            }
        } else if (ratio > settings.start_talking_ratio) {
            memset(szOutFilePath, 0, 255);
            gnSequence++;
            sprintf(szOutFilePath, "./split_%d.wav", gnSequence);
            if (pOutFile)
                sf_close(pOutFile);
            pOutFile = sf_open(szOutFilePath, SFM_WRITE, &stInInfo);
            sf_write_double(pOutFile, buf0, framelen);
            gbTalking = true;
        }
    }

    success = true;

    end:
    if (buf0) free(buf0);
    if (buf1) free(buf1);
    return success;
}

int main(int argc, char *argv[]) {
    int nRetVal;
    const char *szAudioFile;
    SNDFILE *pInFile = NULL;
    SF_INFO stInInfo = {0};
    yt_vad_session *pVadSession;

    if (argc != 2) {
        printf("Usage: %s audioFile(wav)\n", argv[0]);
        goto fail;
    }

    szAudioFile = argv[1];

    pInFile = sf_open(szAudioFile, SFM_READ, &stInInfo);
    if (!pInFile) {
        fprintf(stderr, "Unable to open input file:'%s': %s\n", szAudioFile, sf_strerror(NULL));
        goto fail;
    }

    if (stInInfo.channels != 1) {
        printf("only single-channel wav files supported; input file has %d channels\n", stInInfo.channels);
        goto fail;
    }

    if (!readConfig())
        goto fail;

    printf(szRunningInfo, settings.vad_agn, settings.vad_frame_time_width, settings.vad_correlate, settings.ns_agn,
           settings.check_frame_time_width,
           settings.slide_window_time_width, settings.start_talking_ratio, settings.stop_talking_ratio,
           stInInfo.channels, stInInfo.samplerate);

    pVadSession = yt_webrtc_create_vad_session(settings.vad_agn, settings.vad_frame_time_width, settings.vad_correlate,
                                               1, settings.ns_agn,
                                               stInInfo.samplerate, settings.check_frame_time_width,
                                               settings.slide_window_time_width);

    if (!process_media(pInFile, stInInfo, pVadSession,
                       (size_t) stInInfo.samplerate / 1000 * settings.slide_window_time_width * 2))
        goto fail;

    success:
    nRetVal = 0;
    goto end;

    fail:
    nRetVal = -1;
    goto end;

    end:
    if (pInFile)
        sf_close(pInFile);
    if (pVadSession)
        yt_webrtc_destroy_vad_session(pVadSession);
    return nRetVal;
}
