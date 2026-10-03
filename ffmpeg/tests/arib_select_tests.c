/*
 * Table tests of the pure ARIB audio functions of libffplay (fftools/ffplay_arib_audio.h): the role rules, the
 * per-sample source decision and the starvation rules.
 *   run-arib-select-tests.sh [path of ffplay_arib_audio.h]     (WSL; plain gcc, no FFmpeg needed)
 */
#include <stdio.h>
#include <string.h>

#include "ffplay_arib_audio.h"

static int failures, checks;

#define CHECK_EQ(what, got, want) do { \
    checks++; \
    if ((got) != (want)) { failures++; printf("FAIL %s: got %d want %d\n", what, (int)(got), (int)(want)); } \
} while (0)

typedef struct SelCase {
    const char *name;
    FFPAribSel in;
    int want;
} SelCase;

#define M FFP_ARIB_PREF_MAIN
#define C FFP_ARIB_PREF_COMMENTARY
#define A1 FFP_ARIB_SRC_A1
#define A2 FFP_ARIB_SRC_A2
#define SIL FFP_ARIB_SRC_SILENCE
#define WAIT FFP_ARIB_SRC_WAIT
#define END FFP_ARIB_SRC_END

/* pref, a1 cov/dec, a2 cov/dec, cur_a2, a2_blocked, silence_ok, eof */
static const SelCase sel_cases[] = {
    { "main: audio 1 covers",                          { M, 1, 1, 0, 0, 0, 0, 1, 0 }, A1 },
    { "main: audio 1 covers, audio 2 covers too",      { M, 1, 1, 1, 1, 0, 0, 1, 0 }, A1 },
    { "main: audio 1 covers, audio 2 undecided",       { M, 1, 1, 0, 0, 0, 0, 1, 0 }, A1 },
    { "main: audio 1 hole decided -> silence",         { M, 0, 1, 1, 1, 0, 0, 1, 0 }, SIL },
    { "main: audio 1 undecided -> wait",               { M, 0, 0, 1, 1, 0, 0, 1, 0 }, WAIT },
    { "main: audio 1 hole, silence not justified",     { M, 0, 1, 0, 1, 0, 0, 0, 0 }, WAIT },
    { "main: everything finished -> end",              { M, 0, 1, 0, 1, 0, 0, 1, 1 }, END },
    { "main: audio 1 still covers at eof",             { M, 1, 1, 0, 1, 0, 0, 1, 1 }, A1 },
    { "comm: audio 2 covers",                          { C, 1, 1, 1, 1, 0, 0, 1, 0 }, A2 },
    { "comm: audio 2 covers, audio 1 hole",            { C, 0, 1, 1, 1, 0, 0, 1, 0 }, A2 },
    { "comm: audio 2 covers, audio 1 undecided",       { C, 0, 0, 1, 1, 0, 0, 1, 0 }, A2 },
    { "comm: audio 2 gone, audio 1 covers",            { C, 1, 1, 0, 1, 1, 0, 1, 0 }, A1 },
    { "comm: audio 2 undecided waits (audio 1 covers)",{ C, 1, 1, 0, 0, 0, 0, 1, 0 }, WAIT },
    { "comm: hold keeps audio 1 while audio 2 covers", { C, 1, 1, 1, 1, 0, 1, 1, 0 }, A1 },
    { "comm: hold does not leave audio 2",             { C, 1, 1, 1, 1, 1, 1, 1, 0 }, A2 },
    { "comm: hold, audio 1 hole -> audio 2 plays",     { C, 0, 1, 1, 1, 0, 1, 1, 0 }, A2 },
    { "comm: hold over, audio 2 back",                 { C, 1, 1, 1, 1, 0, 0, 1, 0 }, A2 },
    { "comm: both holes -> silence",                   { C, 0, 1, 0, 1, 0, 0, 1, 0 }, SIL },
    { "comm: audio 2 hole, audio 1 undecided -> wait", { C, 0, 0, 0, 1, 0, 0, 1, 0 }, WAIT },
    { "comm: both holes, no silence yet",              { C, 0, 1, 0, 1, 0, 0, 0, 0 }, WAIT },
    { "comm: end of media",                            { C, 0, 1, 0, 1, 0, 0, 1, 1 }, END },
    { "comm: end of media but audio 2 still covers",   { C, 0, 1, 1, 1, 0, 0, 1, 1 }, A2 },
};

typedef struct RoleCase {
    const char *name;
    FFPAribStream s[6];
    int n, a1, a2;
} RoleCase;

static const RoleCase role_cases[] = {
    { "tags 0x10 / 0x11",                   { { 0, 0, -1 }, { 1, 1, 0x10 }, { 2, 1, 0x11 } }, 3, 1, 2 },
    { "untagged only: the first wins",      { { 1, 1, -1 }, { 2, 1, -1 } }, 2, 1, -1 },
    { "tagged first, untagged later",       { { 1, 1, 0x10 }, { 2, 1, -1 } }, 2, 1, -1 },
    { "untagged first, tag 0x10 later",     { { 1, 1, -1 }, { 2, 1, 0x10 } }, 2, 2, -1 },
    { "the last of 0x10 / 0x83 / 0x85 wins",{ { 1, 1, 0x10 }, { 2, 1, 0x83 }, { 3, 1, 0x85 } }, 3, 3, -1 },
    { "audio 2 only",                       { { 1, 1, 0x11 } }, 1, -1, 1 },
    { "non-AAC ignored",                    { { 0, 0, 0x10 }, { 1, 1, 0x10 } }, 2, 1, -1 },
    { "other tags ignored",                 { { 1, 1, 0x12 }, { 2, 1, 0x10 } }, 2, 2, -1 },
    { "PID order does not matter for 0x11", { { 1, 1, 0x11 }, { 2, 1, 0x10 } }, 2, 2, 1 },
    { "none",                               { { 0, 0, -1 } }, 1, -1, -1 },
};

typedef struct StarveCase {
    const char *name;
    int removed, finished;
    double own, other, video;
    int starved;
    double ku;          /* NAN = don't care, +-INFINITY compared exactly */
} StarveCase;

static const StarveCase starve_cases[] = {
    { "healthy",                         0, 0, 10.0, 10.1, 10.9, 0, NAN },
    { "interleave lag of audio 2 (0.4)", 0, 0, 10.0, 10.4, 10.0, 0, NAN },
    { "lag behind the other audio 0.6",  0, 0, 10.0, 10.6, 10.0, 1, 10.6 - FFP_ARIB_MARGIN_AUDIO_S },
    { "trails the video by 1.4 (CS)",    0, 0, 10.0, 10.0, 11.4, 0, NAN },
    { "lags the video by 1.6",           0, 0, 10.0, 10.0, 11.6, 1, 11.6 - FFP_ARIB_MARGIN_VIDEO_S },
    { "both thresholds, larger known",   0, 0, 10.0, 10.7, 11.6, 1, 10.7 - FFP_ARIB_MARGIN_AUDIO_S },
    { "removed by a PMT",                1, 0, 10.0, 12.0, 12.0, 1, 12.0 - FFP_ARIB_MARGIN_AUDIO_S },
    { "removed, nothing known",          1, 0, NAN, NAN, NAN, 1, -INFINITY },
    { "finished",                        0, 1, 10.0, 10.0, 10.0, 1, INFINITY },
    { "no data yet: unknown",            0, 0, NAN, 10.0, 12.0, 0, NAN },
    { "other unknown, video known",      0, 0, 10.0, NAN, 10.5, 0, NAN },
};

int main(void)
{
    size_t i;
    int a1, a2;

    for (i = 0; i < sizeof(sel_cases) / sizeof(sel_cases[0]); i++)
        CHECK_EQ(sel_cases[i].name, ffp_arib_select(&sel_cases[i].in), sel_cases[i].want);

    for (i = 0; i < sizeof(role_cases) / sizeof(role_cases[0]); i++) {
        ffp_arib_roles(role_cases[i].s, role_cases[i].n, &a1, &a2);
        CHECK_EQ(role_cases[i].name, a1, role_cases[i].a1);
        CHECK_EQ(role_cases[i].name, a2, role_cases[i].a2);
    }

    for (i = 0; i < sizeof(starve_cases) / sizeof(starve_cases[0]); i++) {
        const StarveCase *c = &starve_cases[i];
        double ku = 0;
        int st = ffp_arib_starved(c->removed, c->finished, c->own, c->other, c->video, &ku);
        CHECK_EQ(c->name, st, c->starved);
        if (st && !isnan(c->ku)) {
            checks++;
            if (isinf(c->ku) ? ku != c->ku : fabs(ku - c->ku) > 1e-9) {
                failures++;
                printf("FAIL %s: known_until %g want %g\n", c->name, ku, c->ku);
            }
        }
    }

    /* the thresholds the design numbers rest on */
    CHECK_EQ("T_starve audio 500 ms", (int)(FFP_ARIB_T_STARVE_AUDIO_S * 1000), 500);
    CHECK_EQ("T_starve video 1500 ms", (int)(FFP_ARIB_T_STARVE_VIDEO_S * 1000), 1500);
    CHECK_EQ("hold 1000 ms", (int)(FFP_ARIB_HOLD_S * 1000), 1000);
    CHECK_EQ("margin video > measured CS offset 0.955", FFP_ARIB_MARGIN_VIDEO_S > 0.955, 1);
    CHECK_EQ("margin audio > measured interleave max 0.106", FFP_ARIB_MARGIN_AUDIO_S > 0.106, 1);

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
