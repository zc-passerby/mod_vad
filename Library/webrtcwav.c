#include <yunfan_webrtc_media.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sndfile.h>

int talking = 0;

static bool process_media(SNDFILE *infile, yunfan_vad_session *vad_session, size_t framelen) {
    bool success = false;
    double *buf0 = NULL;
    int16_t *buf1 = NULL;
    int vadres, prev = -1;
    long frames[2] = {0, 0};
    long segments[2] = {0, 0};

    if (framelen > SIZE_MAX / sizeof (double)
            || !(buf0 = malloc(framelen * sizeof *buf0))
            || !(buf1 = malloc(framelen * sizeof *buf1))) {
        fprintf(stderr, "failed to allocate buffers\n");
        goto end;
    }

    while (sf_read_double(infile, buf0, framelen) == (sf_count_t)framelen) {
        for (size_t i = 0; i < framelen; i++)
            buf1[i] = buf0[i] * INT16_MAX;

        int talking_num = 0;
        int vad_res_len = yunfan_webrtc_vad_res_length(vad_session);
        int result[vad_res_len];
        int ret = yunfan_webrtc_vad_operator(vad_session, buf1, framelen, result);
        if (ret < 0) {
            fprintf(stderr, "VAD processing failed\n");
            goto end;
        }
        for(int j = 0; j < vad_res_len; ++j) {
            result[j] = !!result[j];
            vadres = result[j];
            frames[vadres]++;
            if(vadres) talking_num++;
            if (prev != vadres) segments[vadres]++;
            prev = vadres;
        }
        printf("voice detected in %ld of %ld frames (%.2f%%)\n", talking_num, vad_res_len, vad_res_len ? 100.0 * ((double)talking_num / vad_res_len) : 0.0);
    }
    /*
    printf("voice detected in %ld of %ld frames (%.2f%%)\n",
        frames[1], frames[0] + frames[1],
        frames[0] + frames[1] ?
            100.0 * ((double)frames[1] / (frames[0] + frames[1])) : 0.0);
    printf("%ld voice segments, average length %.2f frames\n",
        segments[1], segments[1] ? (double)frames[1] / segments[1] : 0.0);
    printf("%ld non-voice segments, average length %.2f frames\n",
        segments[0], segments[0] ? (double)frames[0] / segments[0] : 0.0);
    */



    success = true;

end:
    if (buf0) free(buf0);
    if (buf1) free(buf1);
    return success;
}

static bool parse_int(int *dest, const char *s, int min, int max)
{
    char *endp;
    long val;

    errno = 0;
    val = strtol(s, &endp, 10);
    if (!errno && !*endp && val >= min && val <= max) {
        *dest = val;
        return true;
    } else {
        return false;
    }
}

int main(int argc, char *argv[])
{
    int retval;
    const char *in_fname;
    SNDFILE *in_sf = NULL, *out_sf[2] = {NULL, NULL};
    SF_INFO in_info = {0}, out_info[2];
    int vad_agn = 0;
    int vad_frame_time_width = 10;
    int vad_correlate = 1;
    int ns_agn = 0;
    int sample_rate = 8000;
    int check_frame_time_width = 100;
    int slide_window_time_width = 200;
    yunfan_vad_session *vad_session = NULL;

    for (int ch = 0; (ch = getopt(argc, argv, "m:f:r:n:s:c:l:h")) != -1;) {
        switch(ch) {
            case 'm':
                if(!parse_int(&vad_agn, optarg, 0, 3)) {
                    fprintf(stderr, "invalid mode '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'f':
                if(!parse_int(&vad_frame_time_width, optarg, 10, 30) || vad_frame_time_width % 10 != 0) {
                    fprintf(stderr, "invalid vad_frame_time_width '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'r':
                if(!parse_int(&vad_correlate, optarg, 1, 3)) {
                    fprintf(stderr, "invalid vad_correlate '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'n':
                if(!parse_int(&ns_agn, optarg, 0, 4)) {
                    fprintf(stderr, "invalid ns_agn '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 's':
                if(!parse_int(&sample_rate, optarg, 8000, 48000) || sample_rate % 8000 != 0) {
                    fprintf(stderr, "invalid sample_rate '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'c':
                if(!parse_int(&check_frame_time_width, optarg, 100, 1000000) || check_frame_time_width % 100 != 0 || check_frame_time_width % vad_frame_time_width != 0) {
                    fprintf(stderr, "invalid check_frame_time_width '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'l':
                if(!parse_int(&slide_window_time_width, optarg, 100, 2000000) || slide_window_time_width % 100 != 0 || check_frame_time_width % slide_window_time_width != 0
                    || slide_window_time_width % vad_frame_time_width != 0 || slide_window_time_width > check_frame_time_width) {
                    fprintf(stderr, "invalid slide_window_time_width '%s'\n", optarg);
                    goto argfail;
                }
                break;
            case 'h':
                printf(
                    "Usage: %s [OPTION]... FILE\n"
                    "Reads FILE in wav format and performs voice activity detection (VAD).\n"
                    "Options:\n"
                    "  -m MODE      set vad mode\n"
                    "  -f WIDTH     set vad_frame_time_width\n"
                    "  -r MODE      set vad_correlate\n"
                    "  -n MODE      set ns mode\n"
                    "  -s RATE      set sample_rate\n"
                    "  -c WIDTH     set check_frame_time_width\n"
                    "  -l WIDTH     set slide_window_time_width\n"
                    "  -h           display this help and exit\n",
                    argv[0]);
                goto success;
                break;
            default:
                goto argfail;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "input file expected\n");
        goto argfail;
    }

    in_fname = argv[optind++];

    if (optind < argc) {
        fprintf(stderr, "unexpected argument '%s'; only one input file expected\n", argv[optind]);
        goto argfail;
    }

    in_sf = sf_open(in_fname, SFM_READ, &in_info);
    if (!in_sf) {
        fprintf(stderr, "Cannot open input file '%s': %s\n", in_fname, sf_strerror(NULL));
        goto fail;
    }

    if (in_info.channels != 1) {
        fprintf(stderr, "only single-channel wav files supported; input file has %d channels\n", in_info.channels);
        goto fail;
    }

    vad_session = yunfan_webrtc_create_vad_session(vad_agn, vad_frame_time_width, vad_correlate, 1, ns_agn, sample_rate, check_frame_time_width, slide_window_time_width);
    if(!vad_session) {
        fprintf(stderr, "out of memory\n");
        goto fail;
    }

    if (!process_media(in_sf, vad_session, (size_t)in_info.samplerate / 1000 * slide_window_time_width * 2))
        goto fail;

success:
    retval = EXIT_SUCCESS;
    goto end;

argfail:
    fprintf(stderr, "Try '%s -h' for more information.\n", argv[0]);
fail:
    retval = EXIT_FAILURE;
    goto end;

end:
    if (in_sf) sf_close(in_sf);
    if (vad_session) yunfan_webrtc_destroy_vad_session(vad_session);

    return retval;
}