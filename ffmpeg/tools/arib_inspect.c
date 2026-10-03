/*
 * arib_inspect: read-only demuxer inspection for the ARIB detection patch
 * (1006a-mpegts-arib-detection) and for interleave-jitter measurement.
 *
 *   arib_inspect [--seconds N] [--start S] [--absent-ms MS] [--jitter] [--quiet] file
 *
 * Opens the file exactly as libffplay does (avformat_open_input + find_stream_info), prints
 * the ARIB verdict, every program (PMT version, markers, stream_index) and every stream (PID,
 * stream_type-derived codec, component_tag), then reads packets for N seconds of media time and
 * reports each change of PMT version / stream list / verdict plus the tsreadex audio roles.
 * With --jitter it also prints the demux interleave statistics.
 * Never writes anything and decodes nothing.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#define MAXS 16

typedef struct Vec { int32_t *v; size_t n, cap; } Vec;

static void vec_push(Vec *a, int32_t x)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 1024;
        a->v = realloc(a->v, a->cap * sizeof(*a->v));
        if (!a->v) exit(2);
    }
    a->v[a->n++] = x;
}

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static void print_dist(const char *label, Vec *a, int32_t absent_ms)
{
    Vec k = {0};
    size_t absent = 0;
    for (size_t i = 0; i < a->n; i++) {
        int32_t x = a->v[i] < 0 ? 0 : a->v[i];
        if (x > absent_ms) { absent++; continue; }
        vec_push(&k, x);
    }
    if (!k.n) {
        printf("  %-28s n=0 absent=%zu\n", label, absent);
        free(k.v);
        return;
    }
    qsort(k.v, k.n, sizeof(*k.v), cmp_i32);
#define P(q) k.v[(size_t)((k.n - 1) * (q))]
    printf("  %-28s n=%zu p50=%d p90=%d p99=%d p99.9=%d max=%d ms  absent(>%dms)=%zu\n", label, k.n,
           P(0.5), P(0.9), P(0.99), P(0.999), k.v[k.n - 1], absent_ms, absent);
    free(k.v);
}

typedef struct SS {
    int used, is_video;
    int64_t first, end;          /* in microseconds, relative to nothing */
    int64_t count, bytes;
    int64_t sum_dur;
    Vec gaps;                    /* gap durations in ms inside this stream */
    int64_t gap_first_us[8];
    int32_t gap_big_ms[8];
    int nbig;
    Vec lag[MAXS];               /* lag[t]: how far t's end trails this stream's end, sampled at each packet of this stream */
} SS;

static SS ss[MAXS];
static int64_t pk_count[MAXS];

static const char *tag_of(AVStream *st, char *buf)
{
    const AVDictionaryEntry *e = av_dict_get(st->metadata, "component_tag", NULL, 0);
    if (!e) return "-";
    snprintf(buf, 16, "0x%02x", atoi(e->value));
    return buf;
}

static int tag_val(AVStream *st)
{
    const AVDictionaryEntry *e = av_dict_get(st->metadata, "component_tag", NULL, 0);
    return e ? atoi(e->value) : -1;
}

static const char *meta(AVDictionary *d, const char *k)
{
    const AVDictionaryEntry *e = av_dict_get(d, k, NULL, 0);
    return e ? e->value : "(unset)";
}

/* tsreadex CServiceFilter::AddPmt: last tag match wins for audio 1, first untagged ADTS only
 * when nothing was chosen yet; audio 2 = tag 0x11. */
static void roles(AVFormatContext *ic, AVProgram *p, int *a1, int *a2)
{
    *a1 = *a2 = -1;
    for (unsigned i = 0; i < p->nb_stream_indexes; i++) {
        AVStream *st = ic->streams[p->stream_index[i]];
        int tag = tag_val(st);
        if (st->codecpar->codec_id != AV_CODEC_ID_AAC)
            continue;
        if ((*a1 < 0 && tag < 0) || tag == 0x10 || tag == 0x83 || tag == 0x85)
            *a1 = st->index;
        else if (tag == 0x11)
            *a2 = st->index;
    }
}

static AVProgram *video_program(AVFormatContext *ic, int *vidx)
{
    *vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (*vidx < 0) return NULL;
    return av_find_program_from_stream(ic, NULL, *vidx);
}

static void print_programs(AVFormatContext *ic, const char *when)
{
    printf("%s: format arib=%s\n", when, meta(ic->metadata, "arib"));
    for (unsigned i = 0; i < ic->nb_programs; i++) {
        AVProgram *p = ic->programs[i];
        printf("%s:  program id=%d pmt_pid=0x%x pmt_version=%d pcr_pid=0x%x arib=%s markers=%s streams=[",
               when, p->id, p->pmt_pid, p->pmt_version, p->pcr_pid, meta(p->metadata, "arib"),
               meta(p->metadata, "arib_markers"));
        for (unsigned k = 0; k < p->nb_stream_indexes; k++)
            printf("%s%u", k ? "," : "", p->stream_index[k]);
        printf("]\n");
    }
}

static void print_roles(AVFormatContext *ic, const char *when)
{
    int vidx, a1, a2;
    AVProgram *vp = video_program(ic, &vidx);
    if (!vp) { printf("%s: roles: no video program\n", when); return; }
    roles(ic, vp, &a1, &a2);
    char b1[16], b2[16];
    printf("%s: roles (program %d): audio1=", when, vp->id);
    if (a1 >= 0) printf("[idx %d pid 0x%x tag %s]", a1, ic->streams[a1]->id, tag_of(ic->streams[a1], b1)); else printf("none");
    printf(" audio2=");
    if (a2 >= 0) printf("[idx %d pid 0x%x tag %s]", a2, ic->streams[a2]->id, tag_of(ic->streams[a2], b2)); else printf("none");
    printf("\n");
}

static void print_streams(AVFormatContext *ic, const char *when, unsigned from)
{
    for (unsigned i = from; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        char b[16];
        const AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        printf("%s:  stream idx=%u pid=0x%x type=%s codec=%s tag=%s ch=%d lang=%s arib_type=%s\n", when, i, st->id,
               av_get_media_type_string(st->codecpar->codec_type), avcodec_get_name(st->codecpar->codec_id),
               tag_of(st, b), st->codecpar->ch_layout.nb_channels, lang ? lang->value : "-",
               meta(st->metadata, "arib_type"));
    }
}

typedef struct Snap { int ver, nb, arib; unsigned idx[64]; } Snap;

static int snap_arib(AVProgram *p)
{
    const AVDictionaryEntry *e = av_dict_get(p->metadata, "arib", NULL, 0);
    return e ? e->value[0] - '0' : -1;
}

static void take(AVProgram *p, Snap *s)
{
    s->ver = p->pmt_version;
    s->nb = p->nb_stream_indexes > 64 ? 64 : p->nb_stream_indexes;
    s->arib = snap_arib(p);
    memcpy(s->idx, p->stream_index, s->nb * sizeof(unsigned));
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* the ANSI argv mangles Japanese names; ffmpeg wants UTF-8 */
    int wargc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        argv = calloc(wargc + 1, sizeof(*argv));
        for (int i = 0; i < wargc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            argv[i] = malloc(n);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
        }
        argc = wargc;
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    double seconds = 30, start = 0;
    int jitter = 0, absent_ms = 3000;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = atof(argv[++i]);
        else if (!strcmp(argv[i], "--absent-ms") && i + 1 < argc) absent_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--jitter")) jitter = 1;
        else file = argv[i];
    }
    if (!file) { fprintf(stderr, "usage: arib_inspect [--seconds N] [--start S] [--absent-ms MS] [--jitter] file\n"); return 2; }

    av_log_set_level(AV_LOG_ERROR);
    AVFormatContext *ic = NULL;
    int ret = avformat_open_input(&ic, file, NULL, NULL);
    if (ret < 0) { printf("OPEN FAILED %d\n", ret); return 1; }
    printf("file: %s\n", file);
    printf("after_open: format arib=%s nb_programs=%u nb_streams=%u\n", meta(ic->metadata, "arib"),
           ic->nb_programs, ic->nb_streams);
    print_programs(ic, "after_open");
    ret = avformat_find_stream_info(ic, NULL);
    if (ret < 0) { printf("find_stream_info FAILED %d\n", ret); return 1; }
    printf("after_probe: nb_programs=%u nb_streams=%u duration=%.1fs\n", ic->nb_programs, ic->nb_streams,
           ic->duration / (double)AV_TIME_BASE);
    print_programs(ic, "after_probe");
    print_streams(ic, "after_probe", 0);
    print_roles(ic, "after_probe");
    printf("verdict: %s\n", !strcmp(meta(ic->metadata, "arib"), "1") ? "ARIB" :
           !strcmp(meta(ic->metadata, "arib"), "0") ? "non-ARIB (PMT evaluated)" : "non-ARIB (no PMT evaluated)");

    if (start > 0)
        avformat_seek_file(ic, -1, INT64_MIN, (int64_t)(start * AV_TIME_BASE), INT64_MAX, 0);

    Snap snaps[32] = {0};
    unsigned nsnap = ic->nb_programs;
    for (unsigned i = 0; i < nsnap && i < 32; i++) take(ic->programs[i], &snaps[i]);
    unsigned known_streams = ic->nb_streams;

    AVPacket *pkt = av_packet_alloc();
    int64_t t0 = AV_NOPTS_VALUE, npk = 0;
    int vidx = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    while (av_read_frame(ic, pkt) >= 0) {
        AVStream *st = ic->streams[pkt->stream_index];
        int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        int64_t us = ts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE : av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q);
        int64_t dur_us = pkt->duration > 0 ? av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q) : 0;
        npk++;
        if (us != AV_NOPTS_VALUE) {
            if (t0 == AV_NOPTS_VALUE) t0 = us;
            if ((us - t0) / 1e6 > seconds) { av_packet_unref(pkt); break; }
        }
        double t = us == AV_NOPTS_VALUE ? -1 : (us - (t0 == AV_NOPTS_VALUE ? 0 : t0)) / 1e6;

        if (ic->nb_streams != known_streams) {
            printf("t=%.3f pkt=%" PRId64 " pos=%" PRId64 ": nb_streams %u -> %u\n", t, npk, pkt->pos, known_streams, ic->nb_streams);
            print_streams(ic, "event", known_streams);
            known_streams = ic->nb_streams;
        }
        for (unsigned i = 0; i < ic->nb_programs && i < 32; i++) {
            AVProgram *p = ic->programs[i];
            Snap n;
            take(p, &n);
            if (i >= nsnap) { snaps[i] = n; nsnap = i + 1; printf("t=%.3f pkt=%" PRId64 ": new program %d\n", t, npk, p->id); print_programs(ic, "event"); continue; }
            if (n.ver != snaps[i].ver || n.nb != snaps[i].nb || n.arib != snaps[i].arib ||
                memcmp(n.idx, snaps[i].idx, n.nb * sizeof(unsigned))) {
                printf("t=%.3f pkt=%" PRId64 " pos=%" PRId64 ": program %d pmt_version %d -> %d arib %d -> %d streams=[", t, npk, pkt->pos,
                       p->id, snaps[i].ver, n.ver, snaps[i].arib, n.arib);
                for (int k = 0; k < n.nb; k++) {
                    char b[16];
                    printf("%s%u(pid 0x%x tag %s)", k ? "," : "", n.idx[k], ic->streams[n.idx[k]]->id, tag_of(ic->streams[n.idx[k]], b));
                }
                printf("]\n");
                snaps[i] = n;
                print_roles(ic, "event");
            }
        }

        int idx = pkt->stream_index;
        if (idx < MAXS) pk_count[idx]++;
        if (jitter && idx < MAXS && us != AV_NOPTS_VALUE) {
            int is_v = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
            int is_a = st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
            if (is_v && idx != vidx) is_v = 0;
            if (is_v || is_a) {
                SS *s = &ss[idx];
                if (!dur_us && is_v) dur_us = 33367;
                if (!s->used) { s->used = 1; s->is_video = is_v; s->first = us; s->end = us + dur_us; }
                else {
                    int64_t gap = us - s->end;
                    if (is_a && gap > 5000) {
                        vec_push(&s->gaps, (int32_t)(gap / 1000));
                        if (s->nbig < 8 && gap > 100000) { s->gap_first_us[s->nbig] = us - t0; s->gap_big_ms[s->nbig++] = (int32_t)(gap / 1000); }
                    }
                    if (us + dur_us > s->end) s->end = us + dur_us;
                }
                s->count++; s->bytes += pkt->size; s->sum_dur += dur_us;
                for (int o = 0; o < MAXS; o++)
                    if (o != idx && ss[o].used)
                        vec_push(&s->lag[o], (int32_t)((s->end - ss[o].end) / 1000));
            }
        }
        av_packet_unref(pkt);
    }
    printf("done: %" PRId64 " packets, %.1fs media\n", npk, seconds);
    print_programs(ic, "final");
    printf("final: nb_streams=%u packets_by_stream:", ic->nb_streams);
    for (unsigned i = 0; i < ic->nb_streams && i < MAXS; i++)
        if (pk_count[i]) printf(" idx%u/pid0x%x=%" PRId64, i, ic->streams[i]->id, pk_count[i]);
    printf("\n");
    print_roles(ic, "final");

    if (jitter) {
        printf("jitter (absent threshold %d ms; lag = how far the named stream's demuxed end PTS trails the other's, sampled at every packet of the other)\n", absent_ms);
        for (int i = 0; i < MAXS; i++) {
            if (!ss[i].used) continue;
            char b[16];
            AVStream *st = ic->streams[i];
            double span = (ss[i].end - ss[i].first) / 1e6;
            printf("stream %d pid=0x%x %s tag=%s packets=%" PRId64 " span=%.1fs covered=%.1fs (%.0f%%)\n", i, st->id,
                   ss[i].is_video ? "video" : avcodec_get_name(st->codecpar->codec_id), tag_of(st, b), ss[i].count, span,
                   ss[i].sum_dur / 1e6, span > 0 ? 100.0 * ss[i].sum_dur / 1e6 / span : 0.0);
            if (!ss[i].is_video) {
                char l[64];
                snprintf(l, sizeof l, "intra-stream gaps >5ms (ms)");
                print_dist(l, &ss[i].gaps, 1 << 30);
                for (int k = 0; k < ss[i].nbig; k++)
                    printf("    gap %d ms at t=%.2fs\n", ss[i].gap_big_ms[k], ss[i].gap_first_us[k] / 1e6);
            }
        }
        for (int i = 0; i < MAXS; i++)
            for (int o = 0; o < MAXS; o++)
                if (ss[i].used && ss[o].used && i != o && ss[i].lag[o].n) {
                    char l[96], bi[16], bo[16];
                    snprintf(l, sizeof l, "lag [%d:%s] behind [%d:%s]", o, ss[o].is_video ? "V" : tag_of(ic->streams[o], bo), i, ss[i].is_video ? "V" : tag_of(ic->streams[i], bi));
                    /* stream i's packet arrived; lag[o] = end_i - end_o: o trails i */
                    print_dist(l, &ss[i].lag[o], absent_ms);
                }
    }
    avformat_close_input(&ic);
    return 0;
}
