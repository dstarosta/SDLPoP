/*
SDLPoP, a port/conversion of the DOS game Prince of Persia.
Copyright (C) 2013-2025  Dávid Nagy

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

The authors of this program may be contacted at https://forum.princed.org

DESCRIPTION:
    MIDI playback routines for SDLPoP.

CREDITS:
    The OPL integration code is based in part on Chocolate Doom's MIDI interface, by Simon Howard (GPLv2-licensed).
    Uses the Nuked OPL3 emulator by Alexey Khokholov (GPLv2-licensed).
    MIDI playback code also published as standalone playback program 'popmidi' by Falcury (GPLv3)
*/

#include "common.h"
#include "opl3.h"
#include "mt32synth.h"
#include "math.h"

#define MAX_MIDI_CHANNELS 16
#define MAX_OPL_VOICES 18

// OPL3 supports more channels (voices) than OPL2: you could increase the number of used voices (try it!)
// However, released notes can linger longer, so for some tracks it will sound too 'washed out'.
// For example, listen to these sounds with 18 voices enabled:
// * The potion music in Potion1.mff and Potion2.mff
// * The hourglass summoning sound in Jaffar.mff
#define NUM_OPL_VOICES 9

extern short midi_playing; // seg009.c
extern SDL_AudioSpec* digi_audiospec; // seg009.c
extern int digi_unavailable; // seg009.c

static opl3_chip opl_chip;
static void* instruments_data;
static instrument_type* instruments;
static int num_instruments;
static byte voice_note[MAX_OPL_VOICES];
static int voice_instrument[MAX_OPL_VOICES];
static int voice_channel[MAX_OPL_VOICES];
static int channel_instrument[MAX_MIDI_CHANNELS];
static int last_used_voice;
static int num_midi_tracks;
static parsed_midi_type parsed_midi;
static midi_track_type* midi_tracks;
static int64_t midi_current_pos; // in MIDI ticks
static float midi_current_pos_fract_part; // partial ticks after the decimal point
static int ticks_to_next_pause; // in MIDI ticks
static dword us_per_beat;
static dword ticks_per_beat;
static int mixing_freq;
static sbyte midi_semitones_higher;
static float current_midi_tempo_modifier;

static bool mt32_channel_has_program[MAX_MIDI_CHANNELS];
static int mt32_ok = 0; // 1 = MT-32 emulator; 0 - fall back to OPL
static int mt32_reset_pending = 0; // an MT-32 song ended

// Tempo adjustments for specific songs:
// * PV scene, with 'Story 3 Jaffar enters':
//   Speed must be exactly right, otherwise it will not line up with the flashing animation in the cutscene.
//   The 'Jaffar enters' song has tempo 705882 (maybe this tempo was carefully fine-tuned)?
// * Intro music: playback speed must match the title appearances/transitions.
const float midi_tempo_modifiers[58] = {
    [sound_53_story_3_Jaffar_comes] = -0.03f, // 3% speedup
    [sound_54_intro_music] = 0.03f, // 3% slowdown
};

// The hardcoded instrument is used as a fallback, if instrument data is not available for some reason.
static instrument_type hardcoded_instrument = {
    0x13, 0x09, 0x04, {{0x02, 0x8D, 0xD7, 0x37, 0x00}, {0x03, 0x03, 0xF5, 0x18, 0x00}}, 0x00, {0x00, 0x00},
};

// Read a variable length integer (max 4 bytes).
static dword midi_read_variable_length(byte** buffer_position) {
    dword result = 0;
    byte* pos = *buffer_position;
    int i;
    for (i = 0; i < 4; ++i) {
        result = (result << 7) | (pos[i] & 0x7F);
        if ((pos[i] & 0x80) == 0) break; // The most significant bit being 0 means that this is the last u8.
    }
    *buffer_position += i + 1; // Advance the pointer, so we know where the next field starts.
    return result;
}

void free_parsed_midi(parsed_midi_type* parsed_midi) {
    for (int i = 0; i < parsed_midi->num_tracks; ++i) {
        free(parsed_midi->tracks[i].events);
    }
    free(parsed_midi->tracks);
    memset(&parsed_midi, 0, sizeof(parsed_midi));
}

bool parse_midi(midi_raw_chunk_type* midi, parsed_midi_type* parsed_midi) {
    parsed_midi->ticks_per_beat = 24;
    if (memcmp(midi->chunk_type, "MThd", 4) != 0) {
        printf("Warning: Tried to play a midi sound without the 'MThd' chunk header.\n");
        return 0;
    }
    if (SDL_SwapBE32(midi->chunk_length) != 6) {
        printf("Warning: Midi file with an invalid header length (expected 6, is %d)\n",
               SDL_SwapBE32(midi->chunk_length));
        return 0;
    }
    word midi_format = SDL_SwapBE16(midi->header.format);
    if (midi_format >= 2) {
        printf("Warning: Unsupported midi format %d (only type 0 or 1 files are supported)\n", midi_format);
        return 0;
    }
    word num_tracks = SDL_SwapBE16(midi->header.num_tracks);
    if (num_tracks < 1) {
        printf("Warning: Midi sound does not have any tracks.\n");
        return 0;
    }
    int division = SDL_SwapBE16(midi->header.time_division);
    if (division < 0) {
        division = (-(division / 256)) * (division & 0xFF); // Translate time delta from the alternative SMTPE format.
    }
    parsed_midi->ticks_per_beat = division;

    parsed_midi->tracks = calloc(1, num_tracks * sizeof(midi_track_type));
    parsed_midi->num_tracks = num_tracks;
    midi_raw_chunk_type* next_track_chunk = (midi_raw_chunk_type*) midi->header.tracks; // The first track chunk starts after the header chunk.
    byte last_event_type = 0;
    for (int track_index = 0; track_index < num_tracks; ++track_index) {
        midi_raw_chunk_type* track_chunk = next_track_chunk;
        if (memcmp(track_chunk->chunk_type, "MTrk", 4) != 0) {
            printf("Warning: midi track without 'MTrk' chunk header.\n");
            free(parsed_midi->tracks);
            memset(&parsed_midi, 0, sizeof(parsed_midi));
            return 0;
        }
        next_track_chunk = (midi_raw_chunk_type*) (track_chunk->data + (dword) SDL_SwapBE32(track_chunk->chunk_length));
        midi_track_type* track = &parsed_midi->tracks[track_index];
        byte* buffer_position = track_chunk->data;
        for (;;) {
            ++track->num_events;
            void* new_track_events = realloc(track->events, track->num_events * sizeof(midi_event_type));
            if (new_track_events == NULL) {
                printf("parse_midi: realloc failed!");
                quit(1);
            }
            track->events = new_track_events;

            midi_event_type* event = &track->events[track->num_events - 1];
            event->delta_time = midi_read_variable_length(&buffer_position);
            event->event_type = *buffer_position;
            if (event->event_type & 0x80) {
                if (event->event_type < 0xF8) last_event_type = event->event_type;
                ++buffer_position;
            } else {
                event->event_type = last_event_type; // Implicit use of the previous event type.
            }
            // Determine the event type and parse the event.
            int num_channel_event_params = 1;
            switch (event->event_type & 0xF0) {
                case 0x80: // note off
                case 0x90: // note on
                case 0xA0: // aftertouch
                case 0xB0: // controller
                case 0xE0: // pitch bend
                    num_channel_event_params = 2; //fallthrough
                case 0xC0: // program change
                case 0xD0: { // channel aftertouch
                    // Read the channel event.
                    event->channel.channel = event->event_type & 0x0F;
                    event->event_type &= 0xF0;
                    event->channel.param1 = *buffer_position++;
                    if (num_channel_event_params == 2) {
                        event->channel.param2 = *buffer_position++;
                    }
                }
                break;
                default:
                    // Not a channel event.
                    switch (event->event_type) {
                        case 0xF0: // SysEx
                        case 0xF7: // SysEx split
                            // Read SysEx event
                            event->sysex.length = midi_read_variable_length(&buffer_position);
                            event->sysex.data = buffer_position;
                            buffer_position += event->sysex.length;
                            break;
                        case 0xFF: // Meta event
                            event->meta.type = *buffer_position++;
                            event->meta.length = midi_read_variable_length(&buffer_position);
                            event->meta.data = buffer_position;
                            buffer_position += event->meta.length;
                            break;
                        default:
                            printf("Warning: unknown midi event type 0x%02x (track %d, event %d)\n",
                                   event->event_type, track_index, track->num_events - 1);
                            free_parsed_midi(parsed_midi);
                            return 0;
                    }
            }
            if (event->event_type == 0xFF /* meta event */ && event->meta.type == 0x2F /* end of track */) {
                break;
            }
            if (buffer_position >= (byte*) next_track_chunk) {
                printf("Error parsing MIDI events (track %d)\n", track_index);
                free_parsed_midi(parsed_midi);
                return 0;
            }

        }

    }

//	printf("Midi file looks good...\n");
    return 1;
}

#if 0
void print_midi_event(int track_index, int event_index, midi_event_type* event) {
    printf("Track %d Event %3d (dt=%4d): ", track_index, event_index, event->delta_time);
    switch (event->event_type) {
        default:
            printf("unknown type (%x)", event->event_type);
            break;
        case 0x80: // note off
            printf("noteoff: ch %d, par %02x|%02x", event->channel.channel, event->channel.param1, event->channel.param2);
            break;
        case 0x90: // note on
            printf("noteon: ch %d, par %02x|%02x", event->channel.channel, event->channel.param1, event->channel.param2);
            {
                float octaves_from_A4 = ((int)event->channel.param1 - 69) / 12.0f;
                float frequency = powf(2.0f,  octaves_from_A4) * 440.0f;
                float f_number_float = frequency * (float)(1 << 20) / 49716.0f;
                int b = (int)(log2f(f_number_float) - 9) & 7;
                int f = ((int)f_number_float >> b) & 1023;
                printf(", freq = %.1f Hz, F=%d, b=%d", frequency, f, b);
            }
            break;
        case 0xA0: // aftertouch
            printf("aftertouch: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
            break;
        case 0xB0: // controller
            printf("controller: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
            break;
        case 0xE0: // pitch bend
            printf("pitch bend: ch %d, par %x|%x", event->channel.channel, event->channel.param1, event->channel.param2);
            break;
        case 0xC0: // program change
            printf("program change: ch %d, par %x", event->channel.channel, event->channel.param1);
            break;
        case 0xD0:
            printf("channel aftertouch: ch %d, par %x", event->channel.channel, event->channel.param1);
            break;
        case 0xF0: // SysEx
            printf("sysex event (length=%d): ", event->sysex.length);
            for (int i = 0; i < event->sysex.length; ++i) {
                printf("%02x ", event->sysex.data[i]);
            }
            break;
        case 0xF7: // SysEx split
            printf("sysex split event");
            // Read SysEx event
            break;
        case 0xFF: // Meta event
            printf("meta: %02x (length=%d): ", event->meta.type, event->meta.length);
            switch(event->meta.type) {
                default:
                    printf("unknown type");
                    break;
                case 0:
                    printf("sequence number");
                    break;
                case 1:
                    printf("text event");
                    break;
                case 2:
                    printf("copyright notice");
                    break;
                case 3:
                    printf("sequence/track name: ");
                    {
                        char* text = malloc(event->meta.length + 1);
                        memcpy(text, event->meta.data, event->meta.length);
                        text[event->meta.length] = '\0';
                        printf("%s", text);
                        free(text);
                    }
                    break;
                case 4:
                    printf("instrument name");
                    break;
                case 0x51:
                    printf("set tempo: ");
                    {
                        byte* data = event->meta.data;
                        int new_tempo = (data[0] << 16) | (data[1] << 8) | (data[2]);
                        printf("set tempo: %d", new_tempo);
                    }
                    break;
                case 0x54:
                    printf("SMTPE offset: ");
                    for (int i = 0; i < event->meta.length; ++i) {
                        printf("%02x ", event->meta.data[i]);
                    }
                    break;
                case 0x58:
                    printf("time signature: ");
                    for (int i = 0; i < event->meta.length; ++i) {
                        printf("%02x ", event->meta.data[i]);
                    }
                    break;
                case 0x2F:
                    printf("end of track");
                    break;
            }
            break;

    }
    putchar('\n');
}
#endif

static byte opl_cached_regs[512];

static void opl_reset(int freq) {
    OPL3_Reset(&opl_chip, freq);
    memset(opl_cached_regs, 0, sizeof(opl_cached_regs));
}

static void opl_write_reg(word reg, byte value) {
    OPL3_WriteReg(&opl_chip, reg, value);
    opl_cached_regs[reg] = value;
}

static void opl_write_reg_masked(word reg, byte value, byte mask) {
    byte cached = opl_cached_regs[reg] & ~mask;
    value = cached | (value & mask);
    opl_write_reg(reg, value);
}


// Reference: https://www.fit.vutbr.cz/~arnost/opl/opl3.html#appendixA
//static u8 adlib_op[] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
static byte sbpro_op[] = { 0,  1,  2,   6,  7,  8,  12, 13, 14, 18, 19, 20,  24, 25, 26,  30, 31, 32};

static word reg_pair_offsets[] = {0x000, 0x001, 0x002, 0x003, 0x004, 0x005,
                                  0x008, 0x009, 0x00A, 0x00B, 0x00C, 0x00D,
                                  0x010, 0x011, 0x012, 0x013, 0x014, 0x015,
                                  0x100, 0x101, 0x102, 0x103, 0x104, 0x105,
                                  0x108, 0x109, 0x10A, 0x10B, 0x10C, 0x10D,
                                  0x110, 0x111, 0x112, 0x113, 0x114, 0x115
                                 };

static word reg_single_offsets[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108};

static word opl_reg_pair_offset(byte voice, byte op) {
    word reg_offset = reg_pair_offsets[sbpro_op[voice]];
    if (op == 1) reg_offset += 3;
    return reg_offset;
}

static void opl_write_instrument(instrument_type* instrument, byte voice) {
    opl_write_reg(0xC0 + reg_single_offsets[voice], instrument->FB_conn | 0x30 /* OPL3: L+R speaker enable */);
    for (byte operator_index = 0; operator_index < 2; ++operator_index) {
        operator_type* operator = &instrument->operators[operator_index];
        word op_reg = opl_reg_pair_offset(voice, operator_index);
        opl_write_reg(0x20 + op_reg, operator->mul);
        opl_write_reg(0x40 + op_reg, operator->ksl_tl);
        opl_write_reg(0x60 + op_reg, operator->a_d);
        opl_write_reg(0x80 + op_reg, operator->s_r);
        opl_write_reg(0xE0 + op_reg, operator->waveform);
    }
}

// A channel that has NOT sent a program change plays on a default, concert-pitch timbre, so
// PoP's play an octave too high unless played on a custom timbre.
// A similar but unrelated issue is happening in OPL as well.
static int normalize_mt32_note(byte channel, byte note) {
    if (channel < MAX_MIDI_CHANNELS && mt32_channel_has_program[channel]) {
        return note;
    }
    int n = (int)note - 12;
    if (n < 0) {
        n = 0;
    }
    return n;
}

static void midi_note_off(midi_event_type* event) {
    byte note = event->channel.param1;
    byte channel = event->channel.channel;

    if (mt32_ok) {
        mt32synth_send_message(0x80 | channel, normalize_mt32_note(channel, note), 0);
        return;
    }

    for (int voice = 0; voice < NUM_OPL_VOICES; ++voice) {
        if (voice_channel[voice] == channel && voice_note[voice] == note) {
            opl_write_reg_masked(0xB0 + reg_single_offsets[voice], 0, 0x20); // release key
            voice_note[voice] = 0; // This voice is now free to be re-used.
            break;
        }
    }
}

static instrument_type* get_instrument(int id) {
    if (id >= 0 && id < num_instruments) {
        return &instruments[id];
    } else {
        return &instruments[0];
    }
}

static void midi_note_on(midi_event_type* event) {
    byte note = event->channel.param1;
    byte velocity = event->channel.param2;
    byte channel = event->channel.channel;

    if (mt32_ok) {
        mt32synth_send_message((velocity == 0 ? 0x80 : 0x90) | channel, normalize_mt32_note(channel, note), velocity);
        return;
    }

    int instrument_id = channel_instrument[channel];
    instrument_type* instrument = get_instrument(instrument_id);

    if (velocity == 0) {
        midi_note_off(event);
    } else {
        // Find a free OPL voice.
        int voice = -1;
        int test_voice = last_used_voice;
        for (int i = 0; i < NUM_OPL_VOICES; ++i) {
            // Don't use the same voice immediately again: that note is probably still be in the release phase.
            ++test_voice;
            test_voice %= NUM_OPL_VOICES;
            if (voice_note[test_voice] == 0) {
                voice = test_voice;
                break;
            }
        }
        last_used_voice = voice;
        if (voice >= 0) {
//			printf("voice %d\n", voice);

            // Set the correct instrument for this voice.
            if (voice_instrument[voice] != instrument_id) {
                opl_write_instrument(instrument, voice);
                voice_instrument[voice] = instrument_id;
            }
            voice_note[voice] = note;
            voice_channel[voice] = channel;

            // Calculate frequency for a MIDI note: note number 69 = A4 = 440 Hz.
            // However, Prince of Persia treats notes as one octave (12 semitones) lower than that, by default.
            // A special MIDI SysEx event is used to change the frequency of all notes.
            float octaves_from_A4 = ((int)event->channel.param1 - 69 - 12 + midi_semitones_higher) / 12.0f;
            float frequency = powf(2.0f,  octaves_from_A4) * 440.0f;
            float f_number_float = frequency * (float)(1 << 20) / 49716.0f;
            int block = (int)(log2f(f_number_float) - 9) & 7;
            int f = ((int)f_number_float >> block) & 1023;
            word reg_offset = reg_single_offsets[voice];
//			opl_write_reg_masked(0xB0 + reg_offset, 0, 0x20); // Turn note off first (should not be necessary)
            opl_write_reg(0xA0 + reg_offset, f & 0xFF);
            opl_write_reg(0xB0 + reg_offset, 0x20 | (block << 2) | (f >> 8));

            // The modulator always uses its own base volume level.
            opl_write_reg_masked(0x40 + opl_reg_pair_offset(voice, 0), instrument->operators[0].ksl_tl, 0x3F);

            // The carrier volume level is calculated as a combination of its base volume and the MIDI note velocity.
            //PRINCE.EXE disassembly: seg009:6C3C
            int instr_volume = instrument->operators[1].ksl_tl & 0x3F;
            int carrier_volume = ((instr_volume + 64) * 225) / (velocity + 161);
            if (carrier_volume < 64) carrier_volume = 64;
            if (carrier_volume > 127) carrier_volume = 127;
            carrier_volume -= 64;
            opl_write_reg_masked(0x40 + opl_reg_pair_offset(voice, 1), carrier_volume, 0x3F);
        } else {
            printf("skipping note, not enough OPL voices\n");
        }

    }

}

static void process_midi_event(midi_event_type* event) {
    switch (event->event_type) {
        case 0x80: // note off
            midi_note_off(event);
            break;
        case 0x90: // note on
            midi_note_on(event);
            break;
        case 0xC0: // program change
            channel_instrument[event->channel.channel] = event->channel.param1;

            if (mt32_ok) {
                if (event->channel.channel < MAX_MIDI_CHANNELS) {
                    mt32_channel_has_program[event->channel.channel] = true;
                }
                mt32synth_send_message(0xC0 | event->channel.channel, event->channel.param1, 0);
            }

            break;
        case 0xF0: { // SysEx event:
            // SDLPoP's private transpose event: manufacturer id "00 00 34", which is NOT a real
            // Roland id — it tells PoP's own driver to shift all notes. It should never be
            // forwarded to the MT-32.
            bool is_pop_transpose = (event->sysex.length == 7 &&
                                     event->sysex.data[0] == 0 &&
                                     event->sysex.data[1] == 0 &&
                                     event->sysex.data[2] == 0x34 &&
                                     (event->sysex.data[3] == 0 || event->sysex.data[3] == 1));
            if (is_pop_transpose) {
                midi_semitones_higher = event->sysex.data[5]; // used only by the OPL path
            }
            if (mt32_ok && !is_pop_transpose) {
                int slen = (int)event->sysex.length;
                byte* sx = malloc((size_t)slen + 1);
                if (sx != NULL) {
                    sx[0] = 0xF0;
                    memcpy(sx + 1, event->sysex.data, (size_t)slen);
                    mt32synth_send_sysex(sx, slen + 1);
                    free(sx);
                }
            }
            break;
        }
        case 0xFF: // Meta event
            switch(event->meta.type) {
                default:
                    break;
                case 0x51: { // set tempo
                    byte* data = event->meta.data;
                    int new_tempo = (data[0] << 16) | (data[1] << 8) | (data[2]);
                    new_tempo *= (1.0f + current_midi_tempo_modifier); // tempo adjustment for specific songs
                    us_per_beat = new_tempo;
                }
                break;
                case 0x54: // SMTPE offset
                    break;
                case 0x58: // time signature
                    break;
                case 0x2F: // end of track
                    break;
            }
            break;
        default:
            break;

    }

}

#define ONE_SECOND_IN_US 1000000LL

void midi_callback(void* userdata, Uint8* stream, int len) {
    if (!midi_playing || len <= 0) return;
    int frames_needed = len / 4;
    while (frames_needed > 0) {
        if (ticks_to_next_pause > 0) {
            // Fill the audio buffer (we have already processed the MIDI events up till this point)
            int64_t us_to_next_pause = ticks_to_next_pause * us_per_beat / ticks_per_beat;
            int64_t us_needed = frames_needed * ONE_SECOND_IN_US / mixing_freq;
            int64_t advance_us = MIN(us_to_next_pause, us_needed);
            int available_frames = (int)(((advance_us * mixing_freq) + ONE_SECOND_IN_US - 1) / ONE_SECOND_IN_US); // round up.
            int advance_frames = MIN(available_frames, frames_needed);
            advance_us = advance_frames * ONE_SECOND_IN_US / mixing_freq; // recalculate, in case the rounding up increased this.
            short* temp_buffer = malloc(advance_frames * 4);
            if (mt32_ok) {
                mt32synth_generate_stream(temp_buffer, advance_frames);
            } else {
                OPL3_GenerateStream(&opl_chip, temp_buffer, advance_frames);
            }
            if (is_sound_on && enable_music) {
                for (int sample = 0; sample < advance_frames * 2; ++sample) {
                    // The line below causes overflows
                    // ((short*)stream)[sample] += temp_buffer[sample];
                    int mixed = (int)((short*)stream)[sample] + (int)temp_buffer[sample];
                    if (mixed > 32767) {
                        mixed = 32767;
                    } else if (mixed < -32768) {
                        mixed = -32768;
                    }
                    ((short*)stream)[sample] = (short)mixed;
                }
            }
            free(temp_buffer);

            frames_needed -= advance_frames;
            stream += advance_frames * 4;
            // Advance the current MIDI tick position.
            // Keep track of the partial ticks that have elapsed so that we do not fall behind.
            float ticks_elapsed_float = (float)advance_us * ticks_per_beat / us_per_beat;
            int64_t ticks_elapsed = (int64_t) ticks_elapsed_float;
            midi_current_pos_fract_part += (ticks_elapsed_float - ticks_elapsed);
            if (midi_current_pos_fract_part > 1.0f) {
                midi_current_pos_fract_part -= 1.0f;
                ticks_elapsed += 1;
            }
            midi_current_pos += ticks_elapsed;
            ticks_to_next_pause -= ticks_elapsed;
        } else {
            // Need to process MIDI events on one or more tracks.
            int num_finished_tracks = 0;
            for (int track_index = 0; track_index < num_midi_tracks; ++track_index) {
                midi_track_type* track = &midi_tracks[track_index];

                while (midi_current_pos >= track->next_pause_tick) {
                    int events_left = track->num_events - track->event_index;
                    if (events_left > 0) {
                        midi_event_type* event = &track->events[track->event_index];
                        track->event_index++;
//						print_midi_event(track_index, track->event_index-1, event);
                        process_midi_event(event);

                        // Need to look ahead: must delay processing of the next event, if there is a pause.
                        if (events_left > 1) {
                            midi_event_type* next_event = &track->events[track->event_index];
                            if (next_event->delta_time != 0) {
                                track->next_pause_tick += next_event->delta_time;
                            }
                        }
                    } else {
                        // reached the last event in this track.
                        ++num_finished_tracks;
                        break;
                    }
                }
            }
            if (num_finished_tracks >= num_midi_tracks) {
                // All tracks have finished. Fill the remaining samples with silence and stop playback.
                SDL_memset(stream, 0, frames_needed * 4);
//				printf("midi_callback(): sound ended\n");
                SDL_LockAudio();
                midi_playing = 0;
                if (mt32_ok) {
                    mt32_reset_pending = 1;
                }
                free_parsed_midi(&parsed_midi);
                SDL_UnlockAudio();
                return;
            } else {
                // Need to delay (let the OPL chip do its work) until one of the tracks needs to process a MIDI event again.
                int64_t first_next_pause_tick = INT64_MAX;
                for (int i = 0; i < num_midi_tracks; ++i) {
                    midi_track_type* track = &midi_tracks[i];
                    if (track->event_index >= track->num_events || midi_current_pos >= track->next_pause_tick) continue;
                    first_next_pause_tick = MIN(first_next_pause_tick, track->next_pause_tick);
                }
                if (first_next_pause_tick == INT64_MAX) {
                    printf("MIDI: Couldn't figure out how long to delay (this is a bug)\n");
                    quit(1);
                }
                ticks_to_next_pause = (int)(first_next_pause_tick - midi_current_pos);
                if (ticks_to_next_pause < 0) {
                    printf("Tried to delay a negative amount of time (this is a bug)\n"); // This should never happen?
                    quit(1);
                }
//				printf("                             delaying %d ticks = %.3f s\n",
//				       ticks_to_next_pause, (((us_per_beat / ticks_per_beat) * (dword)ticks_to_next_pause) / 1e6f));
            }
        }
    }
}


void stop_midi() {
    if (!midi_playing) return;
//	SDL_PauseAudio(1);
    SDL_LockAudio();
    midi_playing = 0;
    if (mt32_ok) {
        mt32synth_all_notes_off();
    }
    free_parsed_midi(&parsed_midi);
    SDL_UnlockAudio();
}

static void* mt32_init_data;
static parsed_midi_type mt32_init_parsed;
static bool mt32_init_parsed_ok;

void free_midi_resources(void) {
    free(instruments_data);
    // MT-32 cleanup - no need for a guard - no-ops
    instruments_data = NULL;
    if (mt32_init_parsed_ok) {
        free_parsed_midi(&mt32_init_parsed);
        mt32_init_parsed_ok = false;
    }
    free(mt32_init_data);
    mt32_init_data = NULL;
    mt32synth_free();
    mt32_ok = 0;
}

// Reads Prince of Persia's MT-32 init resource (id -1) directly from PRINCE.DAT handle, as a
// fallback when the decomposed data/PRINCE/res-1.bin file is not present.
//
// We cannot use "load_from_opendats_alloc". It searches every open DAT (others also carry a -1 entry)
// and compares the index id — stored as the 16-bit value 0xFFFF — against the int -1, which never
// matches.
static void* load_mt32_init_from_dat(dat_type* dat, int* out_size) {
    if (dat == NULL || dat->handle == NULL || dat->dat_table == NULL) {
        return NULL;
    }
    dat_table_type* table = dat->dat_table;
    int count = SDL_SwapLE16(table->res_count);
    for (int i = 0; i < count; ++i) {
        // 0xFFFF == resource id -1
        if (SDL_SwapLE16(table->entries[i].id) != 0xFFFF) {
            continue;
        }
        int size = (int) SDL_SwapLE16(table->entries[i].size);
        if (size <= 4) {
            return NULL;
        }
        if (fseek(dat->handle, SDL_SwapLE32(table->entries[i].offset), SEEK_SET) != 0) {
            return NULL;
        }
        byte checksum;
        if (fread(&checksum, 1, 1, dat->handle) != 1) {
            return NULL;
        }
        void* buf = malloc((size_t) size);
        if (buf == NULL) {
            return NULL;
        }
        if (fread(buf, 1, (size_t) size, dat->handle) != (size_t) size) {
            free(buf);
            return NULL;
        }
        *out_size = size;
        return buf;
    }
    // no resource -1 in this DAT (e.g. a pre-1.3 PRINCE.DAT)
    return NULL;
}

void init_midi() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    instruments = &hardcoded_instrument; // unused if instruments can be loaded normally.
    int size;
    dat_type* dathandle = open_dat("PRINCE.DAT", 0);
    instruments_data = load_from_opendats_alloc(1, "bin", NULL, &size);
    if (!instruments_data) {
        printf("Missing MIDI instruments data (resource 1)\n");
    } else {
        num_instruments = *(byte*)instruments_data;
        if (size == 1 + num_instruments * (int)sizeof(instrument_type)) {
            instruments = (instrument_type*) ((byte*)instruments_data + 1);
        } else {
            printf("MIDI instruments data (resource 1) is not the expected size\n");
            num_instruments = 1;
        }
    }

    // Load PoP's MT-32 custom-timbre init and parse it once.
    // We read it directly from the decomposed resource file data/PRINCE/res-1.bin.
    // The file is the raw resource: a sound tag byte (0x02 = MIDI) followed by the MThd
    // MIDI data.
    {
        const char* init_path = locate_file("data/PRINCE/res-1.bin");
        FILE* initf = fopen(init_path, "rb");
        if (initf != NULL) {
            struct stat st;
            if (fstat(fileno(initf), &st) == 0 && st.st_size > 4) {
                size = (int) st.st_size;
                mt32_init_data = malloc((size_t) size);
                if (mt32_init_data != NULL && fread(mt32_init_data, 1, (size_t) size, initf) != (size_t) size) {
                    free(mt32_init_data);
                    mt32_init_data = NULL;
                }
            }
            fclose(initf);
        }
    }

    // Fallback to resource -1 from PRINCE.DAT
    if (mt32_init_data == NULL) {
        mt32_init_data = load_mt32_init_from_dat(dathandle, &size);
    }

    if (mt32_init_data != NULL) {
        sound_buffer_type* initbuf = (sound_buffer_type*) mt32_init_data;
        mt32_init_parsed_ok = parse_midi((midi_raw_chunk_type*) &initbuf->midi, &mt32_init_parsed);
        if (!mt32_init_parsed_ok) {
            printf("MT-32: could not parse init sequence (res-1.bin)\n");
        }
    }

    if (dathandle != NULL) close_dat(dathandle);
}

// Uploads PoP's custom timbres to the emulated MT-32 by replaying every SysEx in the init
// sequence. Must be sent after each synth reset and before a song, so the MT-32's
// parts/timbres are configured the way the MT32SND music expects. Only the SysEx events
// matter; we send them in order.
static void mt32_flush_sysex(byte* buf, int len, int* sent) {
    if (len > 1 && buf[len - 1] == 0xF7) {
        mt32synth_send_sysex_now(buf, len);
        ++(*sent);
    }
}

// Sends PoP specific instruments to the MT-32 emulator.
static void mt32_send_init(void) {
    if (!mt32_init_parsed_ok) return;

    enum { MT32_SYSEX_MAX = 4096 };
    static byte packet[MT32_SYSEX_MAX];
    int plen = 0;
    bool in_sysex = false;
    int sent = 0;

    for (int t = 0; t < mt32_init_parsed.num_tracks; ++t) {
        midi_track_type* track = &mt32_init_parsed.tracks[t];
        for (int i = 0; i < track->num_events; ++i) {
            midi_event_type* event = &track->events[i];
            int et = event->event_type;
            if (et != 0xF0 && et != 0xF7) {
                continue; // not part of a SysEx
            }

            int slen = (int) event->sysex.length;
            const byte* data = event->sysex.data;

            if (et == 0xF0) {
                // Start of a new SysEx. (If a previous one was somehow left open, drop it.)
                plen = 0;
                in_sysex = true;
                if (plen < MT32_SYSEX_MAX) packet[plen++] = 0xF0;
            } else if (!in_sysex) {
                continue; // stray continuation with no start; ignore
            }

            // Append this fragment's bytes.
            for (int b = 0; b < slen && plen < MT32_SYSEX_MAX; ++b) {
                packet[plen++] = data[b];
            }

            // If this fragment completed the message (ends in 0xF7), flush it.
            if (plen > 0 && packet[plen - 1] == 0xF7) {
                mt32_flush_sysex(packet, plen, &sent);
                plen = 0;
                in_sysex = false;
            }
        }
    }
    (void) sent;
}

// Returns 1 if the real MT-32 emulator is up (MUNT mt32emu DLL + ROMs both present), 0 if we
// must use the OPL/Adlib path. The emulator is brought up once, on first call.
int mt32_available(void) {
    static int mt32_init_tried = 0;
    if (!mt32_init_tried) {
        mt32_init_tried = 1;
        init_digi();
        init_midi();
        if (!digi_unavailable && mt32synth_init(digi_audiospec->freq, MT32_ROM_DIR)) {
            mt32_ok = 1;
            if (mt32_init_parsed_ok) {
                mt32_send_init();
                if (!mt32synth_capture_state()) mt32_ok = 0;
            } else {
                mt32_ok = 0;
            }
            if (!mt32_ok) {
                mt32synth_free();
            }
        } else {
            mt32_ok = 0;
        }
        if (mt32_ok) {
            printf("MT-32: Using Roland emulation for MIDI music.\nDAC: %s, Quality: %d, Sampling Quality: %d, Reverb: %s.\n",
                   get_dac_name(mt32_dac), mt32_quality, mt32_sampling_quality, mt32_reverb ? "on" : "off");
        }
    }
    return mt32_ok;
}

void play_midi_sound(sound_buffer_type* buffer) {
    stop_midi();
    if (buffer == NULL) return;
    init_digi();
    if (digi_unavailable) return;
    init_midi();

    if (!parse_midi((midi_raw_chunk_type*) &buffer->midi, &parsed_midi)) {
        printf("Error reading MIDI music\n");
        return;
    }

    if (mt32_available()) {
        // No need to initialize MT-32 except for clearing reverb on song changes.
        if (mt32_reset_pending) {
            SDL_LockAudio();
            mt32synth_all_notes_off();
            SDL_UnlockAudio();
            mt32_reset_pending = 0;
        }
    } else {
        // Initialize the OPL chip.
        opl_reset(digi_audiospec->freq);
        opl_write_reg(0x105, 0x01); // OPL3 enable (note: the PoP1 Adlib sounds don't actually use OPL3 extensions)
        for (int voice = 0; voice < NUM_OPL_VOICES; ++voice) {
            opl_write_instrument(&instruments[0], voice);
            voice_instrument[voice] = 0;
            voice_note[voice] = 0;
        }
        for (int channel = 0; channel < MAX_MIDI_CHANNELS; channel++) {
            channel_instrument[channel] = channel;
        }
    }

    midi_current_pos = 0;
    midi_current_pos_fract_part = 0;
    ticks_to_next_pause = 0;
    midi_tracks = parsed_midi.tracks;
    num_midi_tracks = parsed_midi.num_tracks;
    midi_semitones_higher = 0;
    // reset MT-32 program channels
    for (int channel = 0; channel < MAX_MIDI_CHANNELS; channel++) {
        mt32_channel_has_program[channel] = false;
    }
    us_per_beat = 500000; // default tempo (500000 us/beat == 120 bpm)
    current_midi_tempo_modifier = midi_tempo_modifiers[current_sound];
    ticks_per_beat = parsed_midi.ticks_per_beat;
    mixing_freq = digi_audiospec->freq;
    midi_playing = 1;
    SDL_PauseAudio(0);
}
