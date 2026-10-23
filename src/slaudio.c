// slaudio: a small OpenSL ES for LEGO Ninjago: Tournament under THUMB.
//
// THUMB doesn't translate OpenSL ES yet, and the game creates its sound engine
// at startup, so without this it crashes there. This covers what the game
// (and most 2010s NDK games) use: PCM buffer-queue players for sound effects
// and AndroidFD players for the MP3 music in the OBB. Everything is mixed in
// software on one thread and played through a Java AudioTrack, reached through
// the JNI bridge THUMB gives guest code.
//
// Shipped inside the APK as lib/armeabi-v7a/libOpenSLES.so. THUMB loads it as
// a sibling of the game's library and resolves the game's OpenSL ES imports
// to it before falling back to its own thunks.

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define DBG_FILE "/sdcard/Android/data/com.lego.ninjago.toe/files/audio.log"
#include "dbglog.h"

#define EXPORT __attribute__((visibility("default")))
#define TAG "slaudio"
#define LOGI(...) dbg_log(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) dbg_log(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// ------------------------------------------------------------ interface IDs

#define DEFINE_IID(name, n)                                                            \
    static const struct SLInterfaceID_ guid_##name = {0x534c4100u + (n), 0x7468, 0x6d62, 0, {0}}; \
    EXPORT const SLInterfaceID SL_IID_##name = &guid_##name;

DEFINE_IID(NULL, 0)
DEFINE_IID(OBJECT, 1)
DEFINE_IID(ENGINE, 2)
DEFINE_IID(PLAY, 3)
DEFINE_IID(VOLUME, 4)
DEFINE_IID(SEEK, 5)
DEFINE_IID(PLAYBACKRATE, 6)
DEFINE_IID(BUFFERQUEUE, 7)
DEFINE_IID(ANDROIDSIMPLEBUFFERQUEUE, 8)
DEFINE_IID(PREFETCHSTATUS, 9)
DEFINE_IID(EFFECTSEND, 10)
DEFINE_IID(ENVIRONMENTALREVERB, 11)
DEFINE_IID(ANDROIDCONFIGURATION, 12)

static int same_iid(SLInterfaceID a, SLInterfaceID b) {
    return a == b || (a && b && memcmp(a, b, sizeof *a) == 0);
}

static const char* iid_name(SLInterfaceID id) {
    static const struct { const SLInterfaceID* id; const char* name; } known[] = {
        {&SL_IID_OBJECT, "OBJECT"},       {&SL_IID_ENGINE, "ENGINE"},
        {&SL_IID_PLAY, "PLAY"},           {&SL_IID_VOLUME, "VOLUME"},
        {&SL_IID_SEEK, "SEEK"},           {&SL_IID_PLAYBACKRATE, "PLAYBACKRATE"},
        {&SL_IID_BUFFERQUEUE, "BUFFERQUEUE"}, {&SL_IID_ANDROIDSIMPLEBUFFERQUEUE, "ANDROIDSIMPLEBUFFERQUEUE"},
        {&SL_IID_PREFETCHSTATUS, "PREFETCHSTATUS"}, {&SL_IID_EFFECTSEND, "EFFECTSEND"},
        {&SL_IID_ENVIRONMENTALREVERB, "ENVIRONMENTALREVERB"}, {&SL_IID_ANDROIDCONFIGURATION, "ANDROIDCONFIGURATION"},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (same_iid(*known[i].id, id)) return known[i].name;
    return "unknown";
}

// ------------------------------------------------------------------ objects

enum { K_ENGINE = 1, K_MIX, K_BQ, K_MP3 };

typedef struct Buf {
    const uint8_t* data;
    SLuint32 size;
} Buf;

typedef struct Obj {
    // Interface handles point at these fields; OBJ() maps them back.
    const struct SLObjectItf_* obj;
    const struct SLEngineItf_* eng;
    const struct SLPlayItf_* play;
    const struct SLVolumeItf_* vol;
    const struct SLAndroidSimpleBufferQueueItf_* bq;
    const struct SLSeekItf_* seek;
    const struct SLPlaybackRateItf_* rate;

    int kind, id, dead;
    SLuint32 state;
    struct Obj* next;

    SLuint32 playState;
    slPlayCallback playCb;
    void* playCtx;
    SLuint32 eventMask;
    int atEnd;

    SLmillibel level;
    SLboolean mute, stereoOn;
    SLpermille stereoPos;
    float gainL, gainR;
    SLpermille speed;

    int channels, bytesPerSample, srcRate;
    double frac;            // position in source frames inside the current buffer / MP3 frame
    uint64_t framesPlayed;  // source frames consumed, for GetPosition

    Buf* bufs;
    SLuint32 maxBufs, head, count, index;
    slAndroidSimpleBufferQueueCallback bqCb;
    void* bqCtx;

    uint8_t* mp3;
    size_t mp3Len, mp3Off;
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcmFrames;
    SLboolean loop;
    SLmillisecond durationMs;
} Obj;

#define OBJ(itf, field) ((Obj*)((char*)(itf) - offsetof(Obj, field)))

static pthread_mutex_t g_lock;
static Obj* g_players;  // every live audio player
static Obj* g_grave;    // destroyed players, freed by the mixer between steps
static JavaVM* g_vm;
static int g_outRate = 48000;
static int g_nextId;
static int g_mixerStarted;
static unsigned g_stat_cbs, g_stat_enq;

__attribute__((constructor)) static void slaudio_init(void) {
    pthread_mutex_init(&g_lock, NULL);
    LOGI("slaudio loaded");
}

static void update_gain(Obj* o) {
    float g = (o->mute || o->level <= SL_MILLIBEL_MIN) ? 0.f : powf(10.f, o->level / 2000.f);
    float l = g, r = g;
    if (o->stereoOn) {
        float p = o->stereoPos / 1000.f;
        if (p > 0) l *= 1.f - p;
        else r *= 1.f + p;
    }
    o->gainL = l;
    o->gainR = r;
}

// ------------------------------------------------------- pending callbacks

typedef struct {
    Obj* o;
    int isPlay;
    SLuint32 event;
    void* cb;
    void* ctx;
} Pending;

#define MAX_PENDING 1024
static Pending g_pending[MAX_PENDING];
static int g_npending;

static void queue_cb(Obj* o, int isPlay, SLuint32 event, void* cb, void* ctx) {
    if (!cb || g_npending >= MAX_PENDING) return;
    g_pending[g_npending++] = (Pending){o, isPlay, event, cb, ctx};
}

// -------------------------------------------------------------- MP3 source

static void mp3_rewind(Obj* o) {
    mp3dec_init(&o->dec);
    o->mp3Off = 0;
    o->pcmFrames = 0;
    o->frac = 0;
    o->framesPlayed = 0;
}

// Decodes the next MP3 frame into o->pcm. Returns 0 at the end of the data.
static int mp3_refill(Obj* o) {
    while (o->mp3Off < o->mp3Len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&o->dec, o->mp3 + o->mp3Off, (int)(o->mp3Len - o->mp3Off), o->pcm, &info);
        if (!info.frame_bytes) return 0;
        o->mp3Off += info.frame_bytes;
        if (samples > 0) {
            o->pcmFrames = samples;
            o->channels = info.channels;
            o->srcRate = info.hz;
            return 1;
        }
    }
    return 0;
}

static SLmillisecond mp3_duration(Obj* o) {
    if (o->durationMs) return o->durationMs;
    mp3dec_t dec;
    mp3dec_init(&dec);
    uint64_t samples = 0;
    int hz = 0;
    size_t off = 0;
    while (off < o->mp3Len) {
        mp3dec_frame_info_t info;
        int s = mp3dec_decode_frame(&dec, o->mp3 + off, (int)(o->mp3Len - off), NULL, &info);
        if (!info.frame_bytes) break;
        off += info.frame_bytes;
        samples += s;
        if (info.hz) hz = info.hz;
    }
    o->durationMs = hz ? (SLmillisecond)(samples * 1000 / hz) : SL_TIME_UNKNOWN;
    return o->durationMs;
}

static void mp3_seek(Obj* o, SLmillisecond ms) {
    mp3_rewind(o);
    if (!mp3_refill(o)) return;
    uint64_t target = (uint64_t)ms * o->srcRate / 1000;
    // Skip whole frames by header only, then decode from there.
    while (o->framesPlayed + o->pcmFrames <= target) {
        o->framesPlayed += o->pcmFrames;
        mp3dec_frame_info_t info;
        int s = 0;
        while (o->mp3Off < o->mp3Len) {
            s = mp3dec_decode_frame(&o->dec, o->mp3 + o->mp3Off, (int)(o->mp3Len - o->mp3Off), NULL, &info);
            if (!info.frame_bytes) break;
            o->mp3Off += info.frame_bytes;
            if (s > 0) break;
        }
        if (s <= 0) {
            o->pcmFrames = 0;
            return;
        }
        o->pcmFrames = s;
    }
    // The frame we stopped in still needs real decoding.
    o->pcmFrames = 0;
    o->frac = (double)(target - o->framesPlayed);
    if (!mp3_refill(o)) o->frac = 0;
}

// ------------------------------------------------------------------ mixing

static inline void pcm_frame(const Obj* o, const uint8_t* d, uint32_t f, float* l, float* r) {
    if (o->bytesPerSample == 2) {
        const int16_t* s = (const int16_t*)d + f * o->channels;
        *l = s[0] * (1.f / 32768.f);
        *r = o->channels > 1 ? s[1] * (1.f / 32768.f) : *l;
    } else {
        const uint8_t* s = d + f * o->channels;
        *l = (s[0] - 128) * (1.f / 128.f);
        *r = o->channels > 1 ? (s[1] - 128) * (1.f / 128.f) : *l;
    }
}

static inline uint32_t buf_frames(const Obj* o, SLuint32 i) {
    return o->bufs[i].size / (uint32_t)(o->channels * o->bytesPerSample);
}

// Retires every buffer the play position has moved past.
static void bq_advance(Obj* o) {
    while (o->count) {
        uint32_t n = buf_frames(o, o->head);
        if (o->frac < n) break;
        o->frac -= n;
        o->framesPlayed += n;
        o->head = (o->head + 1) % o->maxBufs;
        o->count--;
        o->index++;
        queue_cb(o, 0, 0, (void*)o->bqCb, o->bqCtx);
        g_stat_cbs++;
    }
    if (!o->count) o->frac = 0;
}

static void mix_bq(Obj* o, float* acc, int frames) {
    const double step = (double)o->srcRate * o->speed / 1000.0 / g_outRate;
    for (int i = 0; i < frames; i++) {
        bq_advance(o);
        if (!o->count) return;
        const Buf* b = &o->bufs[o->head];
        uint32_t n = buf_frames(o, o->head);
        uint32_t f = (uint32_t)o->frac;
        float l0, r0, l1, r1;
        pcm_frame(o, b->data, f, &l0, &r0);
        if (f + 1 < n) pcm_frame(o, b->data, f + 1, &l1, &r1);
        else l1 = l0, r1 = r0;
        float t = (float)(o->frac - f);
        acc[2 * i] += (l0 + (l1 - l0) * t) * o->gainL;
        acc[2 * i + 1] += (r0 + (r1 - r0) * t) * o->gainR;
        o->frac += step;
    }
    bq_advance(o);
}

static void mix_mp3(Obj* o, float* acc, int frames) {
    for (int i = 0; i < frames; i++) {
        while (o->frac >= o->pcmFrames) {
            o->frac -= o->pcmFrames;
            o->framesPlayed += o->pcmFrames;
            o->pcmFrames = 0;
            if (mp3_refill(o)) continue;
            if (o->loop) {
                mp3_rewind(o);
                if (mp3_refill(o)) continue;
            }
            o->playState = SL_PLAYSTATE_PAUSED;
            o->atEnd = 1;
            if (o->eventMask & SL_PLAYEVENT_HEADATEND)
                queue_cb(o, 1, SL_PLAYEVENT_HEADATEND, (void*)o->playCb, o->playCtx);
            return;
        }
        const double step = (double)o->srcRate * o->speed / 1000.0 / g_outRate;
        uint32_t f = (uint32_t)o->frac;
        const int16_t* s0 = o->pcm + f * o->channels;
        const int16_t* s1 = (int)f + 1 < o->pcmFrames ? s0 + o->channels : s0;
        float t = (float)(o->frac - f);
        float l = (s0[0] + (s1[0] - s0[0]) * t) * (1.f / 32768.f);
        float r = o->channels > 1 ? (s0[1] + (s1[1] - s0[1]) * t) * (1.f / 32768.f) : l;
        acc[2 * i] += l * o->gainL;
        acc[2 * i + 1] += r * o->gainR;
        o->frac += step;
    }
}

static void free_obj(Obj* o) {
    free(o->bufs);
    free(o->mp3);
    free(o);
}

// ------------------------------------------------------------------- output

#define BLOCK 240  // frames per mixing step: 5 ms at 48 kHz, callbacks fire between steps
#define STEPS 4    // steps per AudioTrack write

static int jexc(JNIEnv* env, const char* what) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    LOGW("Java exception during %s", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return 1;
}

typedef struct {
    JNIEnv* env;
    jobject track;
    jshortArray arr;
    jmethodID write;
} Out;

static int out_open(Out* out) {
    JNIEnv* env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK || !env) {
        LOGW("AttachCurrentThread failed");
        return 0;
    }
    out->env = env;
    jclass cls = (*env)->FindClass(env, "android/media/AudioTrack");
    if (jexc(env, "FindClass") || !cls) return 0;
    jvalue a[6];
    memset(a, 0, sizeof a);

    jmethodID mRate = (*env)->GetStaticMethodID(env, cls, "getNativeOutputSampleRate", "(I)I");
    if (!jexc(env, "getNativeOutputSampleRate") && mRate) {
        a[0].i = 3;  // STREAM_MUSIC
        int rate = (*env)->CallStaticIntMethodA(env, cls, mRate, a);
        if (!jexc(env, "getNativeOutputSampleRate()") && rate >= 8000 && rate <= 192000) g_outRate = rate;
    }
    jmethodID mMin = (*env)->GetStaticMethodID(env, cls, "getMinBufferSize", "(III)I");
    int bytes = BLOCK * STEPS * 4 * 3;
    if (!jexc(env, "getMinBufferSize") && mMin) {
        a[0].i = g_outRate;
        a[1].i = 12;  // CHANNEL_OUT_STEREO
        a[2].i = 2;   // ENCODING_PCM_16BIT
        int min = (*env)->CallStaticIntMethodA(env, cls, mMin, a);
        if (!jexc(env, "getMinBufferSize()") && min > bytes) bytes = min;
    }
    jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(IIIIII)V");
    if (jexc(env, "AudioTrack.<init> lookup") || !ctor) return 0;
    a[0].i = 3;
    a[1].i = g_outRate;
    a[2].i = 12;
    a[3].i = 2;
    a[4].i = bytes;
    a[5].i = 1;  // MODE_STREAM
    jobject t = (*env)->NewObjectA(env, cls, ctor, a);
    if (jexc(env, "new AudioTrack") || !t) return 0;
    jmethodID mState = (*env)->GetMethodID(env, cls, "getState", "()I");
    if (mState && (*env)->CallIntMethodA(env, t, mState, a) != 1) {
        jexc(env, "getState");
        LOGW("AudioTrack not initialized");
        return 0;
    }
    jmethodID mPlay = (*env)->GetMethodID(env, cls, "play", "()V");
    out->write = (*env)->GetMethodID(env, cls, "write", "([SII)I");
    if (jexc(env, "play/write lookup") || !mPlay || !out->write) return 0;
    (*env)->CallVoidMethodA(env, t, mPlay, a);
    if (jexc(env, "play")) return 0;
    out->track = (*env)->NewGlobalRef(env, t);
    jshortArray arr = (*env)->NewShortArray(env, BLOCK * STEPS * 2);
    if (jexc(env, "NewShortArray") || !arr) return 0;
    out->arr = (jshortArray)(*env)->NewGlobalRef(env, arr);
    LOGI("AudioTrack open: %d Hz stereo, %d byte buffer", g_outRate, bytes);
    return 1;
}

static void* mixer_main(void* arg) {
    (void)arg;
    static float acc[BLOCK * 2];
    static int16_t pcm[BLOCK * STEPS * 2];
    static Pending local[MAX_PENDING];
    Out out;
    memset(&out, 0, sizeof out);
    int attempts = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    struct timespec lastStat = next;

    for (;;) {
        if (!out.track && g_vm && attempts < 3) {
            attempts++;
            if (!out_open(&out)) {
                LOGW("audio output unavailable (attempt %d), mixing silently", attempts);
                out.track = NULL;
            }
        }

        for (int s = 0; s < STEPS; s++) {
            memset(acc, 0, sizeof acc);
            pthread_mutex_lock(&g_lock);
            while (g_grave) {
                Obj* o = g_grave;
                g_grave = o->next;
                free_obj(o);
            }
            for (Obj* o = g_players; o; o = o->next) {
                if (o->playState != SL_PLAYSTATE_PLAYING || o->state != SL_OBJECT_STATE_REALIZED) continue;
                if (o->kind == K_BQ) mix_bq(o, acc, BLOCK);
                else mix_mp3(o, acc, BLOCK);
            }
            int n = g_npending;
            memcpy(local, g_pending, n * sizeof(Pending));
            g_npending = 0;
            pthread_mutex_unlock(&g_lock);

            for (int i = 0; i < n; i++) {
                Pending* p = &local[i];
                if (p->o->dead) continue;
                if (p->isPlay) ((slPlayCallback)p->cb)((SLPlayItf)&p->o->play, p->ctx, p->event);
                else ((slAndroidSimpleBufferQueueCallback)p->cb)((SLAndroidSimpleBufferQueueItf)&p->o->bq, p->ctx);
            }

            int16_t* dst = pcm + s * BLOCK * 2;
            for (int i = 0; i < BLOCK * 2; i++) {
                float v = acc[i] * 32767.f;
                dst[i] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v;
            }
        }

        if (out.track) {
            JNIEnv* env = out.env;
            (*env)->SetShortArrayRegion(env, out.arr, 0, BLOCK * STEPS * 2, pcm);
            jvalue a[3];
            a[0].l = out.arr;
            a[1].i = 0;
            a[2].i = BLOCK * STEPS * 2;
            int w = (*env)->CallIntMethodA(env, out.track, out.write, a);
            if (jexc(env, "AudioTrack.write") || w < 0) {
                LOGW("AudioTrack.write failed (%d), mixing silently", w);
                out.track = NULL;
            }
            clock_gettime(CLOCK_MONOTONIC, &next);
        } else {
            // No output: keep real time so the game's queues still drain.
            long ns = (long)BLOCK * STEPS * 1000000000LL / g_outRate;
            next.tv_nsec += ns;
            while (next.tv_nsec >= 1000000000L) next.tv_nsec -= 1000000000L, next.tv_sec++;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ahead = (next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
            if (ahead > 0) {
                struct timespec d = {(time_t)(ahead / 1000000000LL), (long)(ahead % 1000000000LL)};
                nanosleep(&d, NULL);
            } else if (ahead < -200000000LL) {
                next = now;
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - lastStat.tv_sec >= 60) {
            lastStat = now;
            int playing = 0, total = 0;
            pthread_mutex_lock(&g_lock);
            for (Obj* o = g_players; o; o = o->next) {
                total++;
                if (o->playState == SL_PLAYSTATE_PLAYING) playing++;
            }
            pthread_mutex_unlock(&g_lock);
            LOGI("stats: %d players (%d playing), %u buffers queued, %u callbacks, output %s", total, playing, g_stat_enq,
                 g_stat_cbs, out.track ? "AudioTrack" : "silent");
        }
    }
    return NULL;
}

static void start_mixer(void) {
    if (g_mixerStarted) return;
    g_mixerStarted = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, mixer_main, NULL) == 0) {
        pthread_detach(t);
        LOGI("mixer thread started");
    } else {
        LOGW("could not start mixer thread");
    }
}

// The engine hands its JNIEnv to AAssetManager_fromJava early on; that is how
// the mixer thread gets to Java. The game reads its assets from the APK and
// OBB itself, so no asset manager is needed.
EXPORT AAssetManager* AAssetManager_fromJava(JNIEnv* env, jobject assetManager) {
    (void)assetManager;
    if (!g_vm && env && (*env)->GetJavaVM(env, &g_vm) == JNI_OK) LOGI("got JavaVM");
    return NULL;
}

// ------------------------------------------------------------------ stubs

#define AS(itf, m, fn) .m = (__typeof__(((struct itf*)0)->m))(fn)
#define UNSUPPORTED(name)                                   \
    static SLresult name(void) {                            \
        LOGW("unsupported OpenSL ES call: %s", #name);      \
        return SL_RESULT_FEATURE_UNSUPPORTED;               \
    }
static SLresult ok_stub(void) { return SL_RESULT_SUCCESS; }

// --------------------------------------------------------------- SLObjectItf

static SLresult Obj_Realize(SLObjectItf self, SLboolean async) {
    (void)async;
    Obj* o = OBJ(self, obj);
    o->state = SL_OBJECT_STATE_REALIZED;
    if (o->kind == K_ENGINE) start_mixer();
    return SL_RESULT_SUCCESS;
}

static SLresult Obj_GetState(SLObjectItf self, SLuint32* pState) {
    if (!pState) return SL_RESULT_PARAMETER_INVALID;
    *pState = OBJ(self, obj)->state;
    return SL_RESULT_SUCCESS;
}

static int player_has(int kind, SLInterfaceID iid) {
    if (same_iid(iid, SL_IID_OBJECT) || same_iid(iid, SL_IID_PLAY) || same_iid(iid, SL_IID_VOLUME) ||
        same_iid(iid, SL_IID_SEEK) || same_iid(iid, SL_IID_PLAYBACKRATE))
        return 1;
    return kind == K_BQ && (same_iid(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE) || same_iid(iid, SL_IID_BUFFERQUEUE));
}

static SLresult Obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void* pInterface) {
    if (!pInterface) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = OBJ(self, obj);
    void* itf = NULL;
    if (same_iid(iid, SL_IID_OBJECT)) itf = &o->obj;
    else if (o->kind == K_ENGINE && same_iid(iid, SL_IID_ENGINE)) itf = &o->eng;
    else if ((o->kind == K_BQ || o->kind == K_MP3) && player_has(o->kind, iid)) {
        if (same_iid(iid, SL_IID_PLAY)) itf = &o->play;
        else if (same_iid(iid, SL_IID_VOLUME)) itf = &o->vol;
        else if (same_iid(iid, SL_IID_SEEK)) itf = &o->seek;
        else if (same_iid(iid, SL_IID_PLAYBACKRATE)) itf = &o->rate;
        else itf = &o->bq;
    }
    if (!itf) {
        LOGW("GetInterface(%s) on object kind %d: not supported", iid_name(iid), o->kind);
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    *(void**)pInterface = itf;
    return SL_RESULT_SUCCESS;
}

static void Obj_AbortAsyncOperation(SLObjectItf self) { (void)self; }

static void Obj_Destroy(SLObjectItf self) {
    Obj* o = OBJ(self, obj);
    if (o->kind != K_BQ && o->kind != K_MP3) {
        free_obj(o);
        return;
    }
    pthread_mutex_lock(&g_lock);
    for (Obj** p = &g_players; *p; p = &(*p)->next) {
        if (*p == o) {
            *p = o->next;
            break;
        }
    }
    o->dead = 1;
    o->next = g_grave;
    g_grave = o;
    pthread_mutex_unlock(&g_lock);
}

static SLresult Obj_GetPriority(SLObjectItf self, SLint32* pPriority, SLboolean* pPreemptable) {
    (void)self;
    if (pPriority) *pPriority = 0;
    if (pPreemptable) *pPreemptable = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLObjectItf_ g_objItf = {
    AS(SLObjectItf_, Realize, Obj_Realize),
    AS(SLObjectItf_, Resume, ok_stub),
    AS(SLObjectItf_, GetState, Obj_GetState),
    AS(SLObjectItf_, GetInterface, Obj_GetInterface),
    AS(SLObjectItf_, RegisterCallback, ok_stub),
    AS(SLObjectItf_, AbortAsyncOperation, Obj_AbortAsyncOperation),
    AS(SLObjectItf_, Destroy, Obj_Destroy),
    AS(SLObjectItf_, SetPriority, ok_stub),
    AS(SLObjectItf_, GetPriority, Obj_GetPriority),
    AS(SLObjectItf_, SetLossOfControlInterfaces, ok_stub),
};

static const struct SLEngineItf_ g_engItf;
static const struct SLPlayItf_ g_playItf;
static const struct SLVolumeItf_ g_volItf;
static const struct SLAndroidSimpleBufferQueueItf_ g_bqItf;
static const struct SLSeekItf_ g_seekItf;
static const struct SLPlaybackRateItf_ g_rateItf;

static Obj* new_obj(int kind) {
    Obj* o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->obj = &g_objItf;
    o->eng = &g_engItf;
    o->play = &g_playItf;
    o->vol = &g_volItf;
    o->bq = &g_bqItf;
    o->seek = &g_seekItf;
    o->rate = &g_rateItf;
    o->kind = kind;
    o->id = ++g_nextId;
    o->state = SL_OBJECT_STATE_UNREALIZED;
    o->playState = SL_PLAYSTATE_STOPPED;
    o->speed = 1000;
    o->channels = 2;
    o->bytesPerSample = 2;
    o->srcRate = 44100;
    update_gain(o);
    return o;
}

// --------------------------------------------------------------- SLEngineItf

static SLresult check_required(int kind, SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    for (SLuint32 i = 0; i < n; i++) {
        if (!ids || !req || !req[i]) continue;
        int ok = kind == K_MIX ? same_iid(ids[i], SL_IID_OBJECT) : player_has(kind, ids[i]);
        if (!ok) {
            LOGW("required interface %s not supported (object kind %d)", iid_name(ids[i]), kind);
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    }
    return SL_RESULT_SUCCESS;
}

static SLresult Eng_CreateOutputMix(SLEngineItf self, SLObjectItf* pMix, SLuint32 n, const SLInterfaceID* ids,
                                    const SLboolean* req) {
    (void)self;
    if (!pMix) return SL_RESULT_PARAMETER_INVALID;
    SLresult r = check_required(K_MIX, n, ids, req);
    if (r != SL_RESULT_SUCCESS) return r;
    Obj* o = new_obj(K_MIX);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *pMix = (SLObjectItf)&o->obj;
    return SL_RESULT_SUCCESS;
}

// Reads the whole MP3 up front: the game may close or reuse the descriptor.
static uint8_t* read_fd(int fd, long long offset, long long length, size_t* outLen) {
    if (length == SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE || length <= 0) {
        long long cur = lseek64(fd, 0, SEEK_CUR);
        long long end = lseek64(fd, 0, SEEK_END);
        lseek64(fd, cur, SEEK_SET);
        length = end - offset;
    }
    if (length <= 0 || length > 64 * 1024 * 1024) return NULL;
    uint8_t* buf = malloc((size_t)length);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < (size_t)length) {
        long long at = offset + (long long)got;
        if (at > 0x7fffffffLL) break;
        ssize_t r = pread(fd, buf + got, (size_t)length - got, (off_t)at);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (!got) {
        free(buf);
        return NULL;
    }
    *outLen = got;
    return buf;
}

static SLresult Eng_CreateAudioPlayer(SLEngineItf self, SLObjectItf* pPlayer, SLDataSource* src, SLDataSink* sink,
                                      SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    (void)self;
    (void)sink;
    if (!pPlayer || !src || !src->pLocator) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 loc = *(const SLuint32*)src->pLocator;
    Obj* o = NULL;

    if (loc == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE || loc == SL_DATALOCATOR_BUFFERQUEUE) {
        const SLDataLocator_AndroidSimpleBufferQueue* l = src->pLocator;
        const SLDataFormat_PCM* f = src->pFormat;
        if (!f || f->formatType != SL_DATAFORMAT_PCM) {
            LOGW("buffer queue player with format %u: not supported", f ? (unsigned)f->formatType : 0u);
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        if (check_required(K_BQ, n, ids, req) != SL_RESULT_SUCCESS) return SL_RESULT_FEATURE_UNSUPPORTED;
        if (f->bitsPerSample != 8 && f->bitsPerSample != 16) {
            LOGW("PCM with %u bits per sample: not supported", (unsigned)f->bitsPerSample);
            return SL_RESULT_CONTENT_UNSUPPORTED;
        }
        o = new_obj(K_BQ);
        if (!o) return SL_RESULT_MEMORY_FAILURE;
        o->maxBufs = l->numBuffers ? l->numBuffers : 1;
        o->bufs = calloc(o->maxBufs, sizeof(Buf));
        o->channels = f->numChannels ? (int)f->numChannels : 1;
        o->bytesPerSample = f->bitsPerSample / 8;
        o->srcRate = f->samplesPerSec >= 1000000 ? (int)(f->samplesPerSec / 1000) : (int)f->samplesPerSec;
        if (o->srcRate <= 0) o->srcRate = 44100;
        LOGI("player %d: PCM buffer queue, %d ch, %d bit, %d Hz, %u buffers", o->id, o->channels,
             o->bytesPerSample * 8, o->srcRate, (unsigned)o->maxBufs);
    } else if (loc == SL_DATALOCATOR_ANDROIDFD) {
        const SLDataLocator_AndroidFD* l = src->pLocator;
        if (check_required(K_MP3, n, ids, req) != SL_RESULT_SUCCESS) return SL_RESULT_FEATURE_UNSUPPORTED;
        size_t len = 0;
        uint8_t* data = read_fd(l->fd, l->offset, l->length, &len);
        if (!data) {
            LOGW("could not read fd %d at %lld (+%lld)", (int)l->fd, (long long)l->offset, (long long)l->length);
            return SL_RESULT_CONTENT_NOT_FOUND;
        }
        o = new_obj(K_MP3);
        if (!o) {
            free(data);
            return SL_RESULT_MEMORY_FAILURE;
        }
        o->mp3 = data;
        o->mp3Len = len;
        mp3_rewind(o);
        LOGI("player %d: MP3 from fd %d, offset %lld, %u bytes", o->id, (int)l->fd, (long long)l->offset,
             (unsigned)len);
    } else {
        LOGW("audio player with data locator 0x%08x: not supported", (unsigned)loc);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    pthread_mutex_lock(&g_lock);
    o->next = g_players;
    g_players = o;
    pthread_mutex_unlock(&g_lock);
    *pPlayer = (SLObjectItf)&o->obj;
    return SL_RESULT_SUCCESS;
}

UNSUPPORTED(Eng_CreateLEDDevice)
UNSUPPORTED(Eng_CreateVibraDevice)
UNSUPPORTED(Eng_CreateAudioRecorder)
UNSUPPORTED(Eng_CreateMidiPlayer)
UNSUPPORTED(Eng_CreateListener)
UNSUPPORTED(Eng_Create3DGroup)
UNSUPPORTED(Eng_CreateMetadataExtractor)
UNSUPPORTED(Eng_CreateExtensionObject)
UNSUPPORTED(Eng_QueryNumSupportedInterfaces)
UNSUPPORTED(Eng_QuerySupportedInterfaces)
UNSUPPORTED(Eng_QuerySupportedExtension)

static SLresult Eng_QueryNumSupportedExtensions(SLEngineItf self, SLuint32* pNum) {
    (void)self;
    if (pNum) *pNum = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Eng_IsExtensionSupported(SLEngineItf self, const SLchar* name, SLboolean* pSupported) {
    (void)self;
    (void)name;
    if (pSupported) *pSupported = SL_BOOLEAN_FALSE;
    return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ g_engItf = {
    AS(SLEngineItf_, CreateLEDDevice, Eng_CreateLEDDevice),
    AS(SLEngineItf_, CreateVibraDevice, Eng_CreateVibraDevice),
    AS(SLEngineItf_, CreateAudioPlayer, Eng_CreateAudioPlayer),
    AS(SLEngineItf_, CreateAudioRecorder, Eng_CreateAudioRecorder),
    AS(SLEngineItf_, CreateMidiPlayer, Eng_CreateMidiPlayer),
    AS(SLEngineItf_, CreateListener, Eng_CreateListener),
    AS(SLEngineItf_, Create3DGroup, Eng_Create3DGroup),
    AS(SLEngineItf_, CreateOutputMix, Eng_CreateOutputMix),
    AS(SLEngineItf_, CreateMetadataExtractor, Eng_CreateMetadataExtractor),
    AS(SLEngineItf_, CreateExtensionObject, Eng_CreateExtensionObject),
    AS(SLEngineItf_, QueryNumSupportedInterfaces, Eng_QueryNumSupportedInterfaces),
    AS(SLEngineItf_, QuerySupportedInterfaces, Eng_QuerySupportedInterfaces),
    AS(SLEngineItf_, QueryNumSupportedExtensions, Eng_QueryNumSupportedExtensions),
    AS(SLEngineItf_, QuerySupportedExtension, Eng_QuerySupportedExtension),
    AS(SLEngineItf_, IsExtensionSupported, Eng_IsExtensionSupported),
};

// ----------------------------------------------------------------- SLPlayItf

static SLresult Play_SetPlayState(SLPlayItf self, SLuint32 st) {
    if (st < SL_PLAYSTATE_STOPPED || st > SL_PLAYSTATE_PLAYING) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = OBJ(self, play);
    pthread_mutex_lock(&g_lock);
    if (st == SL_PLAYSTATE_STOPPED) {
        // Buffers stay queued (the game clears them itself); playback restarts
        // from the start of the current one.
        if (o->kind == K_MP3) mp3_rewind(o);
        else o->frac = 0;
        o->framesPlayed = 0;
        o->atEnd = 0;
    } else if (st == SL_PLAYSTATE_PLAYING && o->kind == K_MP3 && o->atEnd) {
        mp3_rewind(o);
        o->atEnd = 0;
    }
    o->playState = st;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPlayState(SLPlayItf self, SLuint32* pState) {
    if (!pState) return SL_RESULT_PARAMETER_INVALID;
    *pState = OBJ(self, play)->playState;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetDuration(SLPlayItf self, SLmillisecond* pMsec) {
    if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = OBJ(self, play);
    pthread_mutex_lock(&g_lock);
    *pMsec = o->kind == K_MP3 ? mp3_duration(o) : SL_TIME_UNKNOWN;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetPosition(SLPlayItf self, SLmillisecond* pMsec) {
    if (!pMsec) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = OBJ(self, play);
    pthread_mutex_lock(&g_lock);
    double frames = (double)o->framesPlayed + o->frac;
    *pMsec = o->srcRate ? (SLmillisecond)(frames * 1000.0 / o->srcRate) : 0;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_RegisterCallback(SLPlayItf self, slPlayCallback cb, void* ctx) {
    Obj* o = OBJ(self, play);
    pthread_mutex_lock(&g_lock);
    o->playCb = cb;
    o->playCtx = ctx;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Play_SetCallbackEventsMask(SLPlayItf self, SLuint32 mask) {
    OBJ(self, play)->eventMask = mask;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetCallbackEventsMask(SLPlayItf self, SLuint32* pMask) {
    if (!pMask) return SL_RESULT_PARAMETER_INVALID;
    *pMask = OBJ(self, play)->eventMask;
    return SL_RESULT_SUCCESS;
}

static SLresult Play_GetMsecZero(SLPlayItf self, SLmillisecond* p) {
    (void)self;
    if (p) *p = 0;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlayItf_ g_playItf = {
    AS(SLPlayItf_, SetPlayState, Play_SetPlayState),
    AS(SLPlayItf_, GetPlayState, Play_GetPlayState),
    AS(SLPlayItf_, GetDuration, Play_GetDuration),
    AS(SLPlayItf_, GetPosition, Play_GetPosition),
    AS(SLPlayItf_, RegisterCallback, Play_RegisterCallback),
    AS(SLPlayItf_, SetCallbackEventsMask, Play_SetCallbackEventsMask),
    AS(SLPlayItf_, GetCallbackEventsMask, Play_GetCallbackEventsMask),
    AS(SLPlayItf_, SetMarkerPosition, ok_stub),
    AS(SLPlayItf_, ClearMarkerPosition, ok_stub),
    AS(SLPlayItf_, GetMarkerPosition, Play_GetMsecZero),
    AS(SLPlayItf_, SetPositionUpdatePeriod, ok_stub),
    AS(SLPlayItf_, GetPositionUpdatePeriod, Play_GetMsecZero),
};

// --------------------------------------------------------------- SLVolumeItf

static SLresult Vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level) {
    Obj* o = OBJ(self, vol);
    pthread_mutex_lock(&g_lock);
    o->level = level > 0 ? 0 : level;
    update_gain(o);
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetVolumeLevel(SLVolumeItf self, SLmillibel* pLevel) {
    if (!pLevel) return SL_RESULT_PARAMETER_INVALID;
    *pLevel = OBJ(self, vol)->level;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMaxVolumeLevel(SLVolumeItf self, SLmillibel* pMax) {
    (void)self;
    if (!pMax) return SL_RESULT_PARAMETER_INVALID;
    *pMax = 0;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_SetMute(SLVolumeItf self, SLboolean mute) {
    Obj* o = OBJ(self, vol);
    pthread_mutex_lock(&g_lock);
    o->mute = mute;
    update_gain(o);
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetMute(SLVolumeItf self, SLboolean* pMute) {
    if (!pMute) return SL_RESULT_PARAMETER_INVALID;
    *pMute = OBJ(self, vol)->mute;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_EnableStereoPosition(SLVolumeItf self, SLboolean enable) {
    Obj* o = OBJ(self, vol);
    pthread_mutex_lock(&g_lock);
    o->stereoOn = enable;
    update_gain(o);
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_IsEnabledStereoPosition(SLVolumeItf self, SLboolean* pEnable) {
    if (!pEnable) return SL_RESULT_PARAMETER_INVALID;
    *pEnable = OBJ(self, vol)->stereoOn;
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_SetStereoPosition(SLVolumeItf self, SLpermille pos) {
    Obj* o = OBJ(self, vol);
    pthread_mutex_lock(&g_lock);
    o->stereoPos = pos < -1000 ? -1000 : pos > 1000 ? 1000 : pos;
    update_gain(o);
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Vol_GetStereoPosition(SLVolumeItf self, SLpermille* pPos) {
    if (!pPos) return SL_RESULT_PARAMETER_INVALID;
    *pPos = OBJ(self, vol)->stereoPos;
    return SL_RESULT_SUCCESS;
}

static const struct SLVolumeItf_ g_volItf = {
    AS(SLVolumeItf_, SetVolumeLevel, Vol_SetVolumeLevel),
    AS(SLVolumeItf_, GetVolumeLevel, Vol_GetVolumeLevel),
    AS(SLVolumeItf_, GetMaxVolumeLevel, Vol_GetMaxVolumeLevel),
    AS(SLVolumeItf_, SetMute, Vol_SetMute),
    AS(SLVolumeItf_, GetMute, Vol_GetMute),
    AS(SLVolumeItf_, EnableStereoPosition, Vol_EnableStereoPosition),
    AS(SLVolumeItf_, IsEnabledStereoPosition, Vol_IsEnabledStereoPosition),
    AS(SLVolumeItf_, SetStereoPosition, Vol_SetStereoPosition),
    AS(SLVolumeItf_, GetStereoPosition, Vol_GetStereoPosition),
};

// ------------------------------------------------- SLAndroidSimpleBufferQueueItf

static SLresult Bq_Enqueue(SLAndroidSimpleBufferQueueItf self, const void* buf, SLuint32 size) {
    Obj* o = OBJ(self, bq);
    if (!buf && size) return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&g_lock);
    if (o->count >= o->maxBufs) {
        pthread_mutex_unlock(&g_lock);
        return SL_RESULT_BUFFER_INSUFFICIENT;
    }
    SLuint32 tail = (o->head + o->count) % o->maxBufs;
    o->bufs[tail].data = buf;
    o->bufs[tail].size = size;
    o->count++;
    g_stat_enq++;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Bq_Clear(SLAndroidSimpleBufferQueueItf self) {
    Obj* o = OBJ(self, bq);
    pthread_mutex_lock(&g_lock);
    o->head = 0;
    o->count = 0;
    o->frac = 0;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Bq_GetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState* pState) {
    if (!pState) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = OBJ(self, bq);
    pthread_mutex_lock(&g_lock);
    pState->count = o->count;
    pState->index = o->index;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Bq_RegisterCallback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback cb,
                                    void* ctx) {
    Obj* o = OBJ(self, bq);
    pthread_mutex_lock(&g_lock);
    o->bqCb = cb;
    o->bqCtx = ctx;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static const struct SLAndroidSimpleBufferQueueItf_ g_bqItf = {
    AS(SLAndroidSimpleBufferQueueItf_, Enqueue, Bq_Enqueue),
    AS(SLAndroidSimpleBufferQueueItf_, Clear, Bq_Clear),
    AS(SLAndroidSimpleBufferQueueItf_, GetState, Bq_GetState),
    AS(SLAndroidSimpleBufferQueueItf_, RegisterCallback, Bq_RegisterCallback),
};

// ----------------------------------------------------------------- SLSeekItf

static SLresult Seek_SetPosition(SLSeekItf self, SLmillisecond pos, SLuint32 mode) {
    (void)mode;
    Obj* o = OBJ(self, seek);
    pthread_mutex_lock(&g_lock);
    if (o->kind == K_MP3) {
        mp3_seek(o, pos);
        o->atEnd = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Seek_SetLoop(SLSeekItf self, SLboolean enable, SLmillisecond start, SLmillisecond end) {
    (void)start;
    (void)end;
    Obj* o = OBJ(self, seek);
    pthread_mutex_lock(&g_lock);
    o->loop = enable;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Seek_GetLoop(SLSeekItf self, SLboolean* pEnable, SLmillisecond* pStart, SLmillisecond* pEnd) {
    Obj* o = OBJ(self, seek);
    if (pEnable) *pEnable = o->loop;
    if (pStart) *pStart = 0;
    if (pEnd) *pEnd = SL_TIME_UNKNOWN;
    return SL_RESULT_SUCCESS;
}

static const struct SLSeekItf_ g_seekItf = {
    AS(SLSeekItf_, SetPosition, Seek_SetPosition),
    AS(SLSeekItf_, SetLoop, Seek_SetLoop),
    AS(SLSeekItf_, GetLoop, Seek_GetLoop),
};

// --------------------------------------------------------- SLPlaybackRateItf

static SLresult Rate_SetRate(SLPlaybackRateItf self, SLpermille rate) {
    Obj* o = OBJ(self, rate);
    pthread_mutex_lock(&g_lock);
    o->speed = rate < 100 ? 100 : rate > 8000 ? 8000 : rate;
    pthread_mutex_unlock(&g_lock);
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetRate(SLPlaybackRateItf self, SLpermille* pRate) {
    if (!pRate) return SL_RESULT_PARAMETER_INVALID;
    *pRate = OBJ(self, rate)->speed;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetProperties(SLPlaybackRateItf self, SLuint32* pProps) {
    (void)self;
    if (pProps) *pProps = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetCapabilitiesOfRate(SLPlaybackRateItf self, SLpermille rate, SLuint32* pCaps) {
    (void)self;
    (void)rate;
    if (pCaps) *pCaps = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}

static SLresult Rate_GetRateRange(SLPlaybackRateItf self, SLuint8 index, SLpermille* pMin, SLpermille* pMax,
                                  SLpermille* pStep, SLuint32* pCaps) {
    (void)self;
    if (index > 0) return SL_RESULT_PARAMETER_INVALID;
    if (pMin) *pMin = 100;
    if (pMax) *pMax = 8000;
    if (pStep) *pStep = 1;
    if (pCaps) *pCaps = SL_RATEPROP_NOPITCHCORAUDIO;
    return SL_RESULT_SUCCESS;
}

static const struct SLPlaybackRateItf_ g_rateItf = {
    AS(SLPlaybackRateItf_, SetRate, Rate_SetRate),
    AS(SLPlaybackRateItf_, GetRate, Rate_GetRate),
    AS(SLPlaybackRateItf_, SetPropertyConstraints, ok_stub),
    AS(SLPlaybackRateItf_, GetProperties, Rate_GetProperties),
    AS(SLPlaybackRateItf_, GetCapabilitiesOfRate, Rate_GetCapabilitiesOfRate),
    AS(SLPlaybackRateItf_, GetRateRange, Rate_GetRateRange),
};

// ------------------------------------------------------------- entry point

EXPORT SLresult SLAPIENTRY slCreateEngine(SLObjectItf* pEngine, SLuint32 numOptions, const SLEngineOption* pOptions,
                                          SLuint32 numInterfaces, const SLInterfaceID* pIds, const SLboolean* pReq) {
    (void)numOptions;
    (void)pOptions;
    (void)numInterfaces;
    (void)pIds;
    (void)pReq;
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    Obj* o = new_obj(K_ENGINE);
    if (!o) return SL_RESULT_MEMORY_FAILURE;
    *pEngine = (SLObjectItf)&o->obj;
    LOGI("slCreateEngine");
    return SL_RESULT_SUCCESS;
}
