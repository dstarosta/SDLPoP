/*
mt32synth.c: Roland MT-32 / CM-32L music for SDLPoP, via libmt32emu.

Copyright (C) 2026  Dmitry Starosta

Munt mt32emu library is licensed under GPL v2.1.

The code in this file is licensed under GPL v3+.

SDLPoP's MIDI music can be played through a real MT-32 emulator (Munt's libmt32emu, the
same core DOSBox uses) instead of the OPL/Adlib emulator. The game's MIDI bytes — note
on/off, program changes and Prince of Persia's custom-timbre SysEx upload — are fed
straight to the emulator, so it sounds exactly like the original Roland hardware with PoP's
instruments. No sample bank, no pitch-shifting.

The MT-32/CM-32L ROMs are NOT shipped (they are copyrighted). The user drops their own
licensed ROM files into SDLPoP's "data/roms" folder (see MT32_ROM_DIR in config.h),
DOSBox-style. Both a CM-32L pair and an MT-32 pair are accepted; the emulator identifies

*/

#include "mt32synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// We don't link against the DLL at build time. Instead we open it with LoadLibrary/dlopen
// and resolve the handful of functions we use, so a missing DLL is just a graceful "MT-32
// unavailable, use OPL" at run time rather than a build/startup failure.

typedef struct mt32emu_data* mt32emu_context;
typedef const struct mt32emu_data* mt32emu_const_context;

typedef union {
    const void* v0;
} mt32emu_report_handler_i;

// Return codes we check (see mt32emu_return_code in c_types.h).
#define MT32EMU_RC_OK                   0
#define MT32EMU_RC_ADDED_CONTROL_ROM    1
#define MT32EMU_RC_ADDED_PCM_ROM        2

typedef mt32emu_context (*fn_create_context)(mt32emu_report_handler_i report_handler, void* instance_data);
typedef void            (*fn_free_context)(mt32emu_context context);
typedef int             (*fn_add_rom_file)(mt32emu_context context, const char* filename);
typedef void            (*fn_set_analog_output_mode)(mt32emu_context context, int mode);
typedef int             (*fn_open_synth)(mt32emu_const_context context);
typedef void            (*fn_close_synth)(mt32emu_const_context context);
typedef unsigned int    (*fn_get_actual_stereo_output_samplerate)(mt32emu_const_context context);
typedef int             (*fn_play_msg)(mt32emu_const_context context, unsigned int msg);
typedef int             (*fn_play_sysex)(mt32emu_const_context context, const unsigned char* sysex, unsigned int len);
typedef void            (*fn_play_sysex_now)(mt32emu_const_context context, const unsigned char* sysex, unsigned int len);
typedef void            (*fn_render_bit16s)(mt32emu_const_context context, short* stream, unsigned int len);
typedef unsigned int    (*fn_dump_sysex_bank)(mt32emu_const_context context, unsigned char* bank, unsigned int size);
typedef unsigned int    (*fn_apply_sysex_bank)(mt32emu_const_context context, const unsigned char* bank, unsigned int size);

static fn_create_context                       mt32emu_create_context;
static fn_free_context                         mt32emu_free_context;
static fn_add_rom_file                         mt32emu_add_rom_file;
static fn_set_analog_output_mode               mt32emu_set_analog_output_mode;
static fn_open_synth                           mt32emu_open_synth;
static fn_close_synth                          mt32emu_close_synth;
static fn_get_actual_stereo_output_samplerate  mt32emu_get_actual_stereo_output_samplerate;
static fn_play_msg                             mt32emu_play_msg;
static fn_play_sysex                           mt32emu_play_sysex;
static fn_play_sysex_now                       mt32emu_play_sysex_now;
static fn_render_bit16s                        mt32emu_render_bit16s;
static fn_dump_sysex_bank                      mt32emu_dump_sysex_bank;
static fn_apply_sysex_bank                     mt32emu_apply_sysex_bank;

#ifdef _WIN32
#include <windows.h>
typedef HMODULE dynlib_t;
#define DYNLIB_OPEN(name)        LoadLibraryExA((name), NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
#define DYNLIB_SYM(lib, sym)     ((void*)GetProcAddress((lib), (sym)))
#define DYNLIB_CLOSE(lib)        FreeLibrary(lib)
#ifdef _WIN64
static const char* k_dll_names[] = { "libmt32emu-x64.dll", "libmt32emu.dll" }; // Win64
#else
static const char* k_dll_names[] = { "libmt32emu-x86.dll", "libmt32emu.dll" }; // Win32
#endif
#else
#include <dlfcn.h>
typedef void* dynlib_t;
#define DYNLIB_OPEN(name)        dlopen((name), RTLD_NOW | RTLD_LOCAL)
#define DYNLIB_SYM(lib, sym)     dlsym((lib), (sym))
#define DYNLIB_CLOSE(lib)        dlclose(lib)
static const char* k_dll_names[] = {
    "libmt32emu.so.2", "libmt32emu.so",       // Linux / *BSD
    "libmt32emu.2.dylib", "libmt32emu.dylib", // macOS
};
#endif

static dynlib_t dll = NULL;

// Resolves one REQUIRED symbol; sets *ok to 0 on failure. Keeps call sites compact.
static void* resolve(const char* name, int* ok) {
    void* p = DYNLIB_SYM(dll, name);
    if (!p) {
        fprintf(stderr, "mt32synth: DLL missing symbol '%s'\n", name);
        *ok = 0;
    }
    return p;
}

// Tries to open the native libmt32emu library by each known name.
static dynlib_t open_libmt32emu_in(const char* dir) {
    char path[1024];
    for (size_t i = 0; i < sizeof(k_dll_names) / sizeof(k_dll_names[0]); ++i) {
        const char* name = k_dll_names[i];
        if (dir != NULL) {
            snprintf(path, sizeof(path), "%s/%s", dir, name);
            dynlib_t h = DYNLIB_OPEN(path);
            if (h) return h;
        } else {
            dynlib_t h = DYNLIB_OPEN(name); // bare name -> system library search path
            if (h) return h;
        }
    }
    return NULL;
}

// Loads the native libmt32emu library and resolves all functions. Returns 1 on success.
static int load_dll(const char* rom_dir) {
    // already loaded
    if (dll) {
        return 1;
    }

    // 1) Beside the ROMs (cross-OS "just drop it here" location).
    dll = open_libmt32emu_in(rom_dir);

    // 2) Next to the executable (SDL knows where that is on every platform).
    if (!dll) {
        char* base = SDL_GetBasePath();
        if (base) {
            dll = open_libmt32emu_in(base);
            SDL_free(base);
        }
    }

    // 3) The system library search path (installed package).
    if (!dll) {
        dll = open_libmt32emu_in(NULL);
    }

    if (!dll) {
        fprintf(stderr, "mt32synth: libmt32emu not found (OPL music will be used).\n");
        return 0;
    }

    // PORTABILITY: every function resolved below is part of libmt32emu's 2.0+ C API.
    //
    // Note: we require libmt32emu 2.8+; an older DLL lacking a symbol fails to resolve here and the
    // caller falls back to OPL.
    int ok = 1;

    // libmt32emu 2.0+ functions
    mt32emu_create_context                      = (fn_create_context)                      resolve("mt32emu_create_context", &ok);
    mt32emu_free_context                        = (fn_free_context)                        resolve("mt32emu_free_context", &ok);
    mt32emu_add_rom_file                        = (fn_add_rom_file)                        resolve("mt32emu_add_rom_file", &ok);
    mt32emu_set_analog_output_mode              = (fn_set_analog_output_mode)              resolve("mt32emu_set_analog_output_mode", &ok);
    mt32emu_open_synth                          = (fn_open_synth)                          resolve("mt32emu_open_synth", &ok);
    mt32emu_close_synth                         = (fn_close_synth)                         resolve("mt32emu_close_synth", &ok);
    mt32emu_get_actual_stereo_output_samplerate = (fn_get_actual_stereo_output_samplerate) resolve("mt32emu_get_actual_stereo_output_samplerate", &ok);
    mt32emu_play_msg                            = (fn_play_msg)                            resolve("mt32emu_play_msg", &ok);
    mt32emu_play_sysex                          = (fn_play_sysex)                          resolve("mt32emu_play_sysex", &ok);
    mt32emu_play_sysex_now                      = (fn_play_sysex_now)                      resolve("mt32emu_play_sysex_now", &ok);
    mt32emu_render_bit16s                       = (fn_render_bit16s)                       resolve("mt32emu_render_bit16s", &ok);

    // libmt32emu 2.8+ functions for persisting custom timber/instrument state.
    mt32emu_dump_sysex_bank                     = (fn_dump_sysex_bank)                     resolve("mt32emu_dump_sysex_bank", &ok);
    mt32emu_apply_sysex_bank                    = (fn_apply_sysex_bank)                    resolve("mt32emu_apply_sysex_bank", &ok);

    if (!ok) {
        DYNLIB_CLOSE(dll);
        dll = NULL;
        return 0;
    }
    return 1;
}


static mt32emu_context ctx = NULL;
static int synth_rate = 32000;   // native rate the emulator renders at
static int output_rate = 44100;  // SDLPoP's mixing frequency

static unsigned char* state_bank = NULL;
static unsigned int   state_bank_len = 0;

#define MT32_RENDER_CHUNK 1024            // stereo frames pulled from the emulator at a time
static short render_buf[MT32_RENDER_CHUNK * 2];
static int   render_have = 0;             // valid frames currently in render_buf
static short last_frame[2] = {0, 0};      // final frame of the previous chunk (interp overlap)
static int   have_last = 0;               // whether last_frame is valid
static double resample_pos = 0.0;         // fractional frame index into the current chunk

static int load_roms(mt32emu_context c, const char* rom_dir) {
    static const char* groups[][2] = {
        { "CM32L_CONTROL.ROM", "CM32L_PCM.ROM" }, // CM-32L (preferred)
        { "cm32l_control.rom", "cm32l_pcm.rom" },
        { "MT32_CONTROL.ROM",  "MT32_PCM.ROM"  }, // MT-32
        { "mt32_control.rom",  "mt32_pcm.rom"  },
        { "CONTROL.ROM",       "PCM.ROM"       }, // generic
        { "control.rom",       "pcm.rom"       },
    };
    char path[1024];
    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); ++g) {
        int got_control = 0, got_pcm = 0;
        for (int k = 0; k < 2; ++k) {
            snprintf(path, sizeof(path), "%s/%s", rom_dir, groups[g][k]);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            fclose(f);
            int rc = mt32emu_add_rom_file(c, path);
            if (rc == MT32EMU_RC_ADDED_CONTROL_ROM) got_control = 1;
            else if (rc == MT32EMU_RC_ADDED_PCM_ROM) got_pcm = 1;
        }
        if (got_control && got_pcm) {
            return 1;
        }
    }
    return 0;
}

int mt32synth_init(int out_freq, const char* rom_dir) {
    mt32synth_free();
    output_rate = out_freq;

    // Library unavailable - fallback to OPL
    if (!load_dll(rom_dir)) {
        return 0;
    }

    mt32emu_report_handler_i no_handler;
    no_handler.v0 = NULL;

    ctx = mt32emu_create_context(no_handler, NULL);
    if (!ctx) {
        fprintf(stderr, "mt32synth: could not create libmt32emu context (is the DLL present?)\n");
        return 0;
    }

    // No ROMs - using OPL
    if (!load_roms(ctx, rom_dir)) {
        mt32synth_free();
        return 0;
    }
    mt32emu_set_analog_output_mode(ctx, MT32_ANALOG_OUTPUT_MODE);

    if (mt32emu_open_synth(ctx) != MT32EMU_RC_OK) {
        fprintf(stderr, "mt32synth: mt32emu_open_synth failed\n");
        mt32synth_free();
        return 0;
    }

    synth_rate = (int)mt32emu_get_actual_stereo_output_samplerate(ctx);
    if (synth_rate <= 0) synth_rate = 32000;

    render_have = 0;
    resample_pos = 0.0;
    have_last = 0;
    return 1;
}

void mt32synth_free(void) {
    if (ctx) {
        mt32emu_close_synth(ctx);
        mt32emu_free_context(ctx);
        ctx = NULL;
    }
    if (dll) {
        DYNLIB_CLOSE(dll);
        dll = NULL;
        mt32emu_create_context = NULL;
        mt32emu_free_context = NULL;
        mt32emu_add_rom_file = NULL;
        mt32emu_set_analog_output_mode = NULL;
        mt32emu_open_synth = NULL;
        mt32emu_close_synth = NULL;
        mt32emu_get_actual_stereo_output_samplerate = NULL;
        mt32emu_play_msg = NULL;
        mt32emu_play_sysex = NULL;
        mt32emu_play_sysex_now = NULL;
        mt32emu_render_bit16s = NULL;
        mt32emu_dump_sysex_bank = NULL;
        mt32emu_apply_sysex_bank = NULL;
    }
    if (state_bank) {
        free(state_bank);
        state_bank = NULL;
    }
    state_bank_len = 0;
    render_have = 0;
    resample_pos = 0.0;
    have_last = 0;
}

void mt32synth_all_notes_off(void) {
    if (!ctx) return;

    // Close/open is the only thing that reliably clears the previous song's *live* state: still-
    // ringing partials AND the reverb delay lines (everything lighter leaves them alive). It also
    // flushes the internal MIDI queue for free. The catch: it is NOT thread-safe against rendering
    // and it discards the uploaded custom timbres -- so it must run ONLY on the main thread (callers
    // hold the audio lock or have midi_playing==0), and the timbres are restored afterwards.

    mt32emu_close_synth(ctx);

    if (mt32emu_open_synth(ctx) != MT32EMU_RC_OK) {
        fprintf(stderr, "mt32synth: reopen after reset failed\n");
        return;
    }

    if (state_bank != NULL) {
        mt32emu_apply_sysex_bank(ctx, state_bank, state_bank_len); // restore timbres (no re-init)
    }

    render_have = 0;
    resample_pos = 0.0;
    have_last = 0;
}

// Dumps the synth's current memory state (PoP's just-applied init) into state_bank, so it can be
// restored after each close/open without re-sending the init. Call ONCE, right after the init has
// been applied (mt32synth_send_sysex_now makes that synchronous). Returns 1 on success. Safe to
// call again on failure (it retries); a no-op once a bank is already captured.
int mt32synth_capture_state(void) {
    if (!ctx || state_bank != NULL) {
        return state_bank != NULL;
    }
    unsigned int need = mt32emu_dump_sysex_bank(ctx, NULL, 0); // query size
    if (need == 0) {
        return 0;
    }
    state_bank = (unsigned char*)malloc(need);
    if (state_bank == NULL) {
        return 0;
    }
    state_bank_len = mt32emu_dump_sysex_bank(ctx, state_bank, need);
    if (state_bank_len == 0) {
        free(state_bank);
        state_bank = NULL;
        return 0;
    }
    return 1;
}

void mt32synth_send_message(int status, int data1, int data2) {
    if (!ctx) return;

    unsigned int msg = (unsigned int)(status & 0xFF)
                       | ((unsigned int)(data1 & 0xFF) << 8)
                       | ((unsigned int)(data2 & 0xFF) << 16);
    mt32emu_play_msg(ctx, msg);
}

void mt32synth_send_sysex(const unsigned char* data, int length) {
    if (!ctx || length <= 0) {
        return;
    }
    mt32emu_play_sysex(ctx, data, (unsigned int)length);
}

// Like mt32synth_send_sysex but applied synchronously (not queued). Used for PoP's init so it takes
// effect immediately — before the state dump and before the first note.
void mt32synth_send_sysex_now(const unsigned char* data, int length) {
    if (!ctx || length <= 0) {
        return;
    }
    mt32emu_play_sysex_now(ctx, data, (unsigned int)length);
}

// Pulls a fresh chunk of native-rate audio from the emulator, carrying the previous chunk's
// last frame into last_frame[] so interpolation can span the chunk boundary seamlessly.
// After this, resample_pos is relative to the new chunk and may be -1 (meaning "between
// last_frame and render_buf[0]").
static void refill_render_buf(void) {
    if (render_have > 0) {
        last_frame[0] = render_buf[(render_have - 1) * 2 + 0];
        last_frame[1] = render_buf[(render_have - 1) * 2 + 1];
        have_last = 1;
        resample_pos -= (double)render_have; // shift origin to the start of the new chunk
    }
    mt32emu_render_bit16s(ctx, render_buf, MT32_RENDER_CHUNK);
    render_have = MT32_RENDER_CHUNK;
}

static short frame_at(int i, int ch) {
    if (i < 0) {
        return have_last ? last_frame[ch] : render_buf[ch];
    }
    return render_buf[i * 2 + ch];
}

void mt32synth_generate_stream(short* buffer, int num_frames) {
    if (!ctx) {
        memset(buffer, 0, (size_t)num_frames * 2 * sizeof(short));
        return;
    }

    // Same native rate as output: pass through, no resampling.
    if (synth_rate == output_rate) {
        mt32emu_render_bit16s(ctx, buffer, (unsigned int)num_frames);
        return;
    }

    // Linear resample from synth_rate to output_rate. step < 1 when upsampling (44100>32000).
    double step = (double)synth_rate / (double)output_rate;
    for (int frame = 0; frame < num_frames; ++frame) {
        // Ensure both interpolation neighbours (floor(pos) and +1) are available.
        while ((int)floor(resample_pos) + 1 >= render_have) {
            refill_render_buf();
        }
        int idx = (int)floor(resample_pos);
        double frac = resample_pos - (double)idx;
        for (int ch = 0; ch < 2; ++ch) {
            short s0 = frame_at(idx, ch);
            short s1 = frame_at(idx + 1, ch);
            int v = (int)((1.0 - frac) * s0 + frac * s1);
            buffer[frame * 2 + ch] = (short)v;
        }
        resample_pos += step;
    }
}
