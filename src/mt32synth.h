/*
mt32synth.h: Roland MT-32 / CM-32L music for SDLPoP, via libmt32emu.

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

#ifndef MT32SYNTH_H
#define MT32SYNTH_H

#include "common.h"

// Returns the DAC name by its value.
const char* get_dac_name(int value);

// Initializes the emulator and loads the ROMs from rom_dir (e.g. "data/roms").
// Returns 1 on success, 0 if ROMs/DLL are unavailable to fall back on OPL.
int mt32synth_init(int out_freq, const char* rom_dir);

// Frees the emulator. Safe to call even if init failed.
void mt32synth_free(void);

// Clears all sound notes between to prevent reverb when one song starts while another is
// still playing. MUST be called from the main thread only.
void mt32synth_all_notes_off(void);

// Forwards one raw MIDI short message (status, data1, data2) to the emulator.
void mt32synth_send_message(int status, int data1, int data2);

// Forwards a raw SysEx packet (PoP's custom-timbre upload) to the emulator.
void mt32synth_send_sysex(const unsigned char* data, int length);

// Like mt32synth_send_sysex but applied synchronously (not queued for the render thread).
void mt32synth_send_sysex_now(const unsigned char* data, int length);

// Captures the synth's current state (PoP's just-applied init).
// Returns 1 on success (0 if it should be retried next song).
int mt32synth_capture_state(void);

// Renders 'num_frames' stereo frames (interleaved L,R 16-bit) into 'buffer', resampled to
// the mixing frequency. Matches OPL3_GenerateStream so it drops into midi_callback.
void mt32synth_generate_stream(short* buffer, int num_frames);

#endif // MT32SYNTH_H
