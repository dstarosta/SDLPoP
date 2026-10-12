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
licensed ROM files into the folder set by "mt32_rom_path" in SDLPoP.ini (default "roms",
relative to the executable).

*/

#ifndef MT32SYNTH_H
#define MT32SYNTH_H

#include "common.h"

// Returns the DAC name by its value.
const char* get_dac_name(int value);

// Makes a path from an SDLPoP.ini saved in the system's code page UTF-8; empties it if that fails.
void mt32synth_path_to_utf8(char* path, size_t path_size);

// Brings the emulator up with the ROMs in rom_dir. Returns 0 if the library or ROMs are missing (use OPL).
int mt32synth_init(int out_freq, const char* rom_dir);

// Frees the emulator. Safe to call even if init failed.
void mt32synth_free(void);

// Forwards one raw MIDI short message (status, data1, data2) to the emulator.
void mt32synth_send_message(int status, int data1, int data2);

// Applies a raw SysEx packet (PoP's custom-timbre upload) to the emulator at once, not queued.
void mt32synth_send_sysex_now(const unsigned char* data, int length);

// Returns 1 while the emulator still makes sound (notes or reverb), 0 once it is silent.
int mt32synth_is_active(void);

// Empties the reverb, keeping its settings. Only while the audio thread is not rendering from the emulator.
void mt32synth_flush_reverb(void);

// Renders stereo 16-bit frames at the mixing frequency, as OPL3_GenerateStream does for midi_callback.
void mt32synth_generate_stream(short* buffer, int num_frames);

#endif // MT32SYNTH_H
