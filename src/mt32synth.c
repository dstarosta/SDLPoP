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
licensed ROM files into the folder set by "mt32_rom_path" in SDLPoP.ini (default "roms",
relative to the executable).

*/

#include "mt32synth.h"
#include <stdio.h>
#include <string.h>

// The library is opened at run time, not linked, so a missing one only means OPL music.

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
typedef int             (*fn_add_rom_data)(mt32emu_context context, const unsigned char* data, size_t data_size, const void* sha1_digest);
typedef void            (*fn_set_analog_output_mode)(mt32emu_context context, int mode);
typedef void            (*fn_set_dac_input_mode)(mt32emu_const_context context, int mode);
typedef void            (*fn_set_samplerate_conversion_quality)(mt32emu_context context, int quality);
typedef void            (*fn_set_reverb_overridden)(mt32emu_const_context context, int overridden);
typedef void            (*fn_set_reverb_enabled)(mt32emu_const_context context, int enabled);
typedef int             (*fn_open_synth)(mt32emu_const_context context);
typedef void            (*fn_close_synth)(mt32emu_const_context context);
typedef unsigned int    (*fn_get_actual_stereo_output_samplerate)(mt32emu_const_context context);
typedef int             (*fn_play_msg)(mt32emu_const_context context, unsigned int msg);
typedef void            (*fn_play_sysex_now)(mt32emu_const_context context, const unsigned char* sysex, unsigned int len);
typedef void            (*fn_render_bit16s)(mt32emu_const_context context, short* stream, unsigned int len);
typedef int             (*fn_is_active)(mt32emu_const_context context);
typedef unsigned int    (*fn_get_library_version_int)(void);

#define MT32EMU_MIN_VERSION 0x00020800 // 2.8.0, as 0x00MMmmpp: older libraries have bugs

static fn_create_context                       mt32emu_create_context;
static fn_free_context                         mt32emu_free_context;
static fn_add_rom_data                         mt32emu_add_rom_data;
static fn_set_analog_output_mode               mt32emu_set_analog_output_mode;
static fn_set_dac_input_mode                   mt32emu_set_dac_input_mode;
static fn_set_samplerate_conversion_quality    mt32emu_set_samplerate_conversion_quality;
static fn_set_reverb_overridden                mt32emu_set_reverb_overridden;
static fn_set_reverb_enabled                   mt32emu_set_reverb_enabled;
static fn_open_synth                           mt32emu_open_synth;
static fn_close_synth                          mt32emu_close_synth;
static fn_get_actual_stereo_output_samplerate  mt32emu_get_actual_stereo_output_samplerate;
static fn_play_msg                             mt32emu_play_msg;
static fn_play_sysex_now                       mt32emu_play_sysex_now;
static fn_render_bit16s                        mt32emu_render_bit16s;
static fn_is_active                            mt32emu_is_active;

#ifdef _WIN32
#include <windows.h>
typedef HMODULE dynlib_t;
// Paths are UTF-8, as everywhere in the game, and LOAD_WITH_ALTERED_SEARCH_PATH needs a full path.
static HMODULE open_library_utf8(const char* name) {
	WCHAR wide_name[POP_MAX_PATH];
	WCHAR full_path[POP_MAX_PATH];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, POP_MAX_PATH) == 0) {
		return NULL;
	}
	// A bare name is left as it is, for the system's own search.
	if (wcspbrk(wide_name, L"/\\") == NULL) {
		return LoadLibraryW(wide_name);
	}
	DWORD full_len = GetFullPathNameW(wide_name, POP_MAX_PATH, full_path, NULL);
	if (full_len == 0 || full_len >= POP_MAX_PATH) {
		return NULL;
	}
	return LoadLibraryExW(full_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
#define DYNLIB_OPEN(name)        open_library_utf8(name)
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

void mt32synth_path_to_utf8(char* path, size_t path_size) {
#ifdef _WIN32
	// A path that is not valid UTF-8 comes from an SDLPoP.ini saved in the system's code page.
	WCHAR wide_path[POP_MAX_PATH];
	char utf8_path[POP_MAX_PATH];
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0) != 0) {
		return;
	}
	if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide_path, POP_MAX_PATH) == 0) {
		path[0] = '\0';
		return;
	}
	int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, utf8_path, sizeof(utf8_path), NULL, NULL);
	if (utf8_len == 0 || (size_t)utf8_len > path_size) {
		path[0] = '\0';
		return;
	}
	memcpy(path, utf8_path, (size_t)utf8_len);
#else
	(void)path;
	(void)path_size;
#endif
}

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
	char path[POP_MAX_PATH];
	for (size_t i = 0; i < sizeof(k_dll_names) / sizeof(k_dll_names[0]); ++i) {
		const char* name = k_dll_names[i];
		if (dir != NULL) {
			snprintf(path, sizeof(path), "%s/%s", dir, name);
			dynlib_t handle = DYNLIB_OPEN(path);
			if (handle) {
				return handle;
			}
		} else {
			dynlib_t handle = DYNLIB_OPEN(name); // bare name -> system library search path
			if (handle) {
				return handle;
			}
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
		const char* base = SDL_GetBasePath();
		if (base) {
			dll = open_libmt32emu_in(base);
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

	// A library lacking one of these functions, or older than 2.8, is turned down: OPL is used.
	int ok = 1;

	fn_get_library_version_int get_library_version = (fn_get_library_version_int) resolve("mt32emu_get_library_version_int", &ok);
	if (ok && get_library_version() < MT32EMU_MIN_VERSION) {
		fprintf(stderr, "mt32synth: libmt32emu 2.8 or newer is required (OPL music will be used).\n");
		ok = 0;
	}

	mt32emu_create_context                      = (fn_create_context)                      resolve("mt32emu_create_context", &ok);
	mt32emu_free_context                        = (fn_free_context)                        resolve("mt32emu_free_context", &ok);
	mt32emu_add_rom_data                        = (fn_add_rom_data)                        resolve("mt32emu_add_rom_data", &ok);
	mt32emu_set_analog_output_mode              = (fn_set_analog_output_mode)              resolve("mt32emu_set_analog_output_mode", &ok);
	mt32emu_set_dac_input_mode                  = (fn_set_dac_input_mode)                  resolve("mt32emu_set_dac_input_mode", &ok);
	mt32emu_set_samplerate_conversion_quality   = (fn_set_samplerate_conversion_quality)   resolve("mt32emu_set_samplerate_conversion_quality", &ok);
	mt32emu_set_reverb_overridden               = (fn_set_reverb_overridden)               resolve("mt32emu_set_reverb_overridden", &ok);
	mt32emu_set_reverb_enabled                  = (fn_set_reverb_enabled)                  resolve("mt32emu_set_reverb_enabled", &ok);
	mt32emu_open_synth                          = (fn_open_synth)                          resolve("mt32emu_open_synth", &ok);
	mt32emu_close_synth                         = (fn_close_synth)                         resolve("mt32emu_close_synth", &ok);
	mt32emu_get_actual_stereo_output_samplerate = (fn_get_actual_stereo_output_samplerate) resolve("mt32emu_get_actual_stereo_output_samplerate", &ok);
	mt32emu_play_msg                            = (fn_play_msg)                            resolve("mt32emu_play_msg", &ok);
	mt32emu_play_sysex_now                      = (fn_play_sysex_now)                      resolve("mt32emu_play_sysex_now", &ok);
	mt32emu_render_bit16s                       = (fn_render_bit16s)                       resolve("mt32emu_render_bit16s", &ok);
	mt32emu_is_active                           = (fn_is_active)                           resolve("mt32emu_is_active", &ok);

	if (!ok) {
		DYNLIB_CLOSE(dll);
		dll = NULL;
		return 0;
	}
	return 1;
}


static mt32emu_context ctx = NULL;
static int output_rate = DIGI_SAMPLE_RATE;  // SDLPoP's mixing frequency
static int synth_rate = DIGI_SAMPLE_RATE;  // The emulator's own frequency (set by the analog output mode)

#define MT32_RENDER_CHUNK 1024            // stereo frames pulled from the emulator at a time
static short render_buf[MT32_RENDER_CHUNK * 2];
static SDL_AudioStream* resample_stream = NULL; // synth_rate -> output_rate; NULL when they match

typedef struct rom_search_type {
	const char* wanted;
	char found[POP_MAX_PATH]; // the name as it is on disk; empty until found
} rom_search_type;

static SDL_EnumerationResult SDLCALL match_rom_file(void* userdata, const char* dirname, const char* fname) {
	rom_search_type* search = (rom_search_type*)userdata;
	if (SDL_strcasecmp(fname, search->wanted) != 0) {
		return SDL_ENUM_CONTINUE;
	}
	snprintf(search->found, sizeof(search->found), "%s", fname);
	return SDL_ENUM_SUCCESS;
}

// Builds the path of the ROM file named 'wanted', ignoring case, in the ROM folder. Returns 1 if it is there.
static int find_rom_file(const char* rom_dir, const char* wanted, char* path, size_t path_size) {
	rom_search_type search = { .wanted = wanted };
	if (!SDL_EnumerateDirectory(rom_dir, match_rom_file, &search) || search.found[0] == '\0') {
		return 0;
	}
	return snprintf(path, path_size, "%s/%s", rom_dir, search.found) < (int)path_size;
}

// ROM images handed to the library, which uses them in place: they are kept until it is freed.
#define MT32_MAX_ROM_IMAGES 8
static void* rom_images[MT32_MAX_ROM_IMAGES];
static int rom_image_count = 0;

// Gives a ROM file's contents to the library, whose own loading cannot open non-ASCII paths on Windows.
static int add_rom(mt32emu_context c, const char* path) {
	if (rom_image_count >= MT32_MAX_ROM_IMAGES) {
		return 0;
	}
	size_t size = 0;
	void* image = SDL_LoadFile(path, &size);
	if (image == NULL) {
		return 0;
	}
	int result = mt32emu_add_rom_data(c, (const unsigned char*)image, size, NULL);
	if (result > 0) {
		rom_images[rom_image_count++] = image;
	} else {
		SDL_free(image);
	}
	return result;
}

static int try_load_pair(mt32emu_context c, const char* rom_dir,
                         const char* ctrl, const char* pcm) {
	char path_ctrl[POP_MAX_PATH];
	char path_pcm[POP_MAX_PATH];

	if (!find_rom_file(rom_dir, ctrl, path_ctrl, sizeof(path_ctrl))) {
		return 0;
	}
	if (!find_rom_file(rom_dir, pcm, path_pcm, sizeof(path_pcm))) {
		return 0;
	}
	if (add_rom(c, path_ctrl) != MT32EMU_RC_ADDED_CONTROL_ROM) {
		return 0;
	}
	if (add_rom(c, path_pcm) != MT32EMU_RC_ADDED_PCM_ROM) {
		return 0;
	}
	return 1;
}


static int load_roms(mt32emu_context c, const char* rom_dir) {
	static const char* const variants[][2] = {
		{"mt32_control.rom",  "mt32_pcm.rom"},  // MT-32
		{"cm32l_control.rom", "cm32l_pcm.rom"}, // CM-32L
		{"control.rom",       "pcm.rom"},       // generic
	};

	enum { MT32 = 0, CM32L = 1, GENERIC = 2 };

	int order[3];
	const int mt32_first = mt32_dac == 2;

	order[0] = mt32_first ? MT32 : CM32L;
	order[1] = mt32_first ? CM32L : MT32;
	order[2] = GENERIC;

	for (int group = 0; group < 3; ++group) {
		const char* ctrl = variants[order[group]][0];
		const char* pcm = variants[order[group]][1];
		if (try_load_pair(c, rom_dir, ctrl, pcm)) {
			printf("ROM: %s, %s.\n", ctrl, pcm);
			return 1;
		}
	}

	return 0;
}

const char* get_dac_name(int value) {
	if (value < 0) {
		return "OPL";
	}
	switch (value) {
		case 1:
			return "PURE";
		case 2:
			return "GENERATION1";
		case 3:
			return "GENERATION2";
		default:
			return "NICE";
	}
}

static void mt32synth_apply_reverb_pref(void) {
	if (!ctx || mt32_reverb) {
		return;
	}
	mt32emu_set_reverb_overridden(ctx, 1); // ignore PoP's reverb SysEx
	mt32emu_set_reverb_enabled(ctx, 0);    // keep reverb off
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

	// A value outside the documented range is not passed on: the library does not check it.
	int analog_mode = (mt32_quality >= 0 && mt32_quality <= 3) ? mt32_quality : 2;
	int dac_mode = (mt32_dac >= 0 && mt32_dac <= 3) ? mt32_dac : 0;

	mt32emu_set_analog_output_mode(ctx, analog_mode);

	// The library's own rate converter is kept out of the way: too slow, or only linear at its fastest.
	mt32emu_set_samplerate_conversion_quality(ctx, 0);

	if (mt32emu_open_synth(ctx) != MT32EMU_RC_OK) {
		fprintf(stderr, "mt32synth: mt32emu_open_synth failed\n");
		mt32synth_free();
		return 0;
	}

	mt32emu_set_dac_input_mode(ctx, dac_mode);

	mt32synth_apply_reverb_pref();

	synth_rate = (int)mt32emu_get_actual_stereo_output_samplerate(ctx);

	if (synth_rate <= 0) {
		synth_rate = output_rate;
	}

	if (synth_rate != output_rate) {
		SDL_AudioSpec synth_spec = { .format = SDL_AUDIO_S16, .channels = 2, .freq = synth_rate };
		SDL_AudioSpec output_spec = { .format = SDL_AUDIO_S16, .channels = 2, .freq = output_rate };
		resample_stream = SDL_CreateAudioStream(&synth_spec, &output_spec);
		if (resample_stream == NULL) {
			fprintf(stderr, "mt32synth: SDL_CreateAudioStream failed: %s\n", SDL_GetError());
			mt32synth_free();
			return 0;
		}
	}

	return 1;
}

void mt32synth_free(void) {
	if (ctx) {
		mt32emu_close_synth(ctx);
		mt32emu_free_context(ctx);
		ctx = NULL;
	}
	while (rom_image_count > 0) {
		SDL_free(rom_images[--rom_image_count]);
	}
	if (dll) {
		DYNLIB_CLOSE(dll);
		dll = NULL;
		mt32emu_create_context = NULL;
		mt32emu_free_context = NULL;
		mt32emu_add_rom_data = NULL;
		mt32emu_set_analog_output_mode = NULL;
		mt32emu_set_dac_input_mode = NULL;
		mt32emu_set_samplerate_conversion_quality = NULL;
		mt32emu_set_reverb_overridden = NULL;
		mt32emu_set_reverb_enabled = NULL;
		mt32emu_open_synth = NULL;
		mt32emu_close_synth = NULL;
		mt32emu_get_actual_stereo_output_samplerate = NULL;
		mt32emu_play_msg = NULL;
		mt32emu_play_sysex_now = NULL;
		mt32emu_render_bit16s = NULL;
		mt32emu_is_active = NULL;
	}
	if (resample_stream) {
		SDL_DestroyAudioStream(resample_stream);
		resample_stream = NULL;
	}
}

void mt32synth_send_message(int status, int data1, int data2) {
	if (!ctx) {
		return;
	}

	unsigned int msg = (unsigned int)(status & 0xFF)
	                   | ((unsigned int)(data1 & 0xFF) << 8)
	                   | ((unsigned int)(data2 & 0xFF) << 16);
	mt32emu_play_msg(ctx, msg);
}

// Applies a SysEx packet at once (not queued): PoP's init, which must be in place before the first note.
void mt32synth_send_sysex_now(const unsigned char* data, int length) {
	if (!ctx || length <= 0) {
		return;
	}
	mt32emu_play_sysex_now(ctx, data, (unsigned int)length);
}

int mt32synth_is_active(void) {
	return ctx != NULL && mt32emu_is_active(ctx);
}

void mt32synth_flush_reverb(void) {
	if (!ctx || !mt32_reverb) {
		return;
	}
	// Off and on again: the reverb comes back empty, with the settings PoP gave it.
	mt32emu_set_reverb_enabled(ctx, 0);
	mt32emu_set_reverb_enabled(ctx, 1);
}

void mt32synth_generate_stream(short* buffer, int num_frames) {
	const int frame_size = 2 * (int)sizeof(short);
	const int wanted = num_frames * frame_size;

	if (!ctx || num_frames <= 0) {
		memset(buffer, 0, (size_t)(num_frames > 0 ? wanted : 0));
		return;
	}

	// The emulator already renders at the mixing frequency.
	if (resample_stream == NULL) {
		mt32emu_render_bit16s(ctx, buffer, (unsigned int)num_frames);
		return;
	}

	// Only what the converter still needs is rendered, so the notes keep their timing; the limit is a safeguard.
	for (int pass = 0; pass < 64; ++pass) {
		int available = SDL_GetAudioStreamAvailable(resample_stream);
		if (available < 0 || available >= wanted) {
			break;
		}
		int missing = (wanted - available) / frame_size;
		int render_frames = (int)(((int64_t)missing * synth_rate + output_rate - 1) / output_rate);
		if (render_frames < 1) {
			render_frames = 1;
		} else if (render_frames > MT32_RENDER_CHUNK) {
			render_frames = MT32_RENDER_CHUNK;
		}
		mt32emu_render_bit16s(ctx, render_buf, (unsigned int)render_frames);
		if (!SDL_PutAudioStreamData(resample_stream, render_buf, render_frames * frame_size)) {
			break;
		}
	}

	int filled = SDL_GetAudioStreamData(resample_stream, buffer, wanted);
	if (filled < 0) {
		filled = 0;
	}
	if (filled < wanted) {
		memset((byte*)buffer + filled, 0, (size_t)(wanted - filled));
	}
}
