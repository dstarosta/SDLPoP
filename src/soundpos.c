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
*/

// Positional stereo: the digitized sounds are heard from where they happen, for a listener at the middle of the screen.
// The other rooms are placed by the room links. This only reads the game's state, it never changes it.

#include "common.h"
#include <math.h>

#ifdef USE_POSITIONAL_STEREO

#define ROOM_WIDTH 320
#define ROOM_HEIGHT 192 // (a falling tile moves by 192 from a room to the one below)
#define PAN_DEPTH 240.0f // the listener's distance from the screen: pan = dx / sqrt(dx^2 + depth^2)
#define NEAR_RADIUS 160.0f // no loss of volume closer than this to the middle of the screen
#define DB_PER_ROOM 5.0f // the loss beyond it, by a room's width
#define MIN_GAIN_DB -24.0f
#define SELF_PAN 0.5f // the kid's own sounds: half the pan
#define ILD_MAX_DB 10.0f // the far ear's drop for a sound fully to one side
#define ITD_MAX_MS 0.3f // and its delay
#define GLIDE_MS 20.0f // how fast a new place is reached
#define DELAY_RING 64 // (the delay line: at least ITD_MAX_MS at 192 kHz)

extern short digi_playing; // seg009.c
extern int digi_unavailable; // seg009.c
extern SDL_AudioSpec* digi_audiospec; // seg009.c
extern SDL_AudioStream* digi_audio_stream; // seg009.c

static sound_src_type next_sound_src; // the source of next_sound
static sound_src_type digi_src; // the source of the digitized sound playing

// Where a sound played now comes from: the tile being animated (process_trobs), the falling tile being moved (do_mobs),
// otherwise Char. In a cutscene: Char's place on the screen.
sound_src_type sound_src_now(void) {
	sound_src_type src = {0};
	if (sound_src_from == sound_src_from_trob) {
		if (trob.tilepos >= 30) return src;
		src.room = trob.room;
		src.x = trob.tilepos % 10 * 32 + 16;
		src.y = trob.tilepos / 10 * 63 + 32;
	} else if (sound_src_from == sound_src_from_mob) {
		src.room = curmob.room;
		src.x = curmob.xh * 8 + 16;
		src.y = curmob.y;
	} else {
		src.room = Char.room;
		src.x = (Char.x - 58) * 32 / 14; // (x_bump: column 0 starts at 58, 14 per column)
		src.y = Char.y - 32;
		src.self = (Char.charid == charid_0_kid);
		if (is_cutscene) {
			src.kind = src_screen;
			return src;
		}
	}
	src.kind = src.room ? src_room : src_none;
	return src;
}

// The room's place from drawn_room in rooms, by the shortest way through the links.
// Returns 1; 0 if the links do not reach it; -1 if there is no room on the screen.
static int room_offset(int room, int* dx, int* dy) {
	static const sbyte step_x[4] = {-1, 1, 0, 0};
	static const sbyte step_y[4] = {0, 0, -1, 1};
	int n = MIN(level.used_rooms, ROOMCOUNT);
	if (drawn_room < 1 || drawn_room > n) return -1;
	if (room < 1 || room > n) return 0;
	sbyte ox[ROOMCOUNT + 1];
	sbyte oy[ROOMCOUNT + 1];
	byte seen[ROOMCOUNT + 1] = {0};
	byte queue[ROOMCOUNT + 1];
	int head = 0;
	int tail = 0;
	seen[drawn_room] = 1;
	ox[drawn_room] = 0;
	oy[drawn_room] = 0;
	queue[tail++] = drawn_room;
	while (head < tail) {
		int r = queue[head++];
		if (r == room) {
			*dx = ox[r];
			*dy = oy[r];
			return 1;
		}
		link_type links = level.roomlinks[r - 1];
		byte next[4] = {links.left, links.right, links.up, links.down};
		for (int k = 0; k < 4; ++k) {
			int t = next[k];
			if (t >= 1 && t <= n && !seen[t]) {
				seen[t] = 1;
				ox[t] = ox[r] + step_x[k];
				oy[t] = oy[r] + step_y[k];
				queue[tail++] = t;
			}
		}
	}
	return 0;
}

static float pan_of(float dx) {
	return dx / sqrtf(dx * dx + PAN_DEPTH * PAN_DEPTH);
}

// Pan -1 (left) .. 1 (right) and gain (1: as recorded).
void sound_src_place(const sound_src_type* src, float* pan, float* gain) {
	*pan = 0;
	*gain = 1;
	if (src->kind == src_screen) {
		*pan = pan_of(src->x - ROOM_WIDTH / 2.0f) * (src->self ? SELF_PAN : 1);
		return;
	}
	if (src->kind != src_room) return;
	int rx = 0;
	int ry = 0;
	int reached = room_offset(src->room, &rx, &ry);
	if (reached < 0) return;
	if (reached == 0) {
		// Not reached by the links: away from the screen, far.
		*gain = powf(10, MIN_GAIN_DB / 20);
		return;
	}
	float dx = rx * ROOM_WIDTH + src->x - ROOM_WIDTH / 2.0f;
	float dy = ry * ROOM_HEIGHT + src->y - ROOM_HEIGHT / 2.0f;
	*pan = pan_of(dx) * (src->self ? SELF_PAN : 1);
	float distance = sqrtf(dx * dx + dy * dy);
	float loss = distance > NEAR_RADIUS ? -DB_PER_ROOM * (distance - NEAR_RADIUS) / ROOM_WIDTH : 0;
	*gain = powf(10, MAX(loss, MIN_GAIN_DB) / 20);
}

// play_sound: next_sound's source. The same sound again keeps the nearer one's place.
void queue_sound_src(int sound_id) {
	sound_src_type src = sound_src_now();
	if (sound_id == next_sound) {
		float pan, gain_new, gain_old;
		sound_src_place(&src, &pan, &gain_new);
		sound_src_place(&next_sound_src, &pan, &gain_old);
		if (gain_new < gain_old) src = next_sound_src;
	}
	next_sound_src = src;
}

// play_next_sound: the sound it starts comes from next_sound's source (else from nowhere: centred).
void use_queued_sound_src(bool use) {
	sound_start_src = use ? next_sound_src : (sound_src_type){0};
}

// The place of the digitized sound: each ear's gain and delay, and the targets they glide to.
static float place_gain[2] = {1, 1};
static float place_delay[2] = {0, 0};
static float target_gain[2] = {1, 1};
static float target_delay[2] = {0, 0};
static int delay_ring[DELAY_RING];
static unsigned ring_pos;

static void set_digi_place(float pan, float gain, bool at_once) {
	pan = MAX(-1.0f, MIN(1.0f, pan));
	float side = fabsf(pan);
	float drop = powf(10, -ILD_MAX_DB * side / 20);
	float delay = ITD_MAX_MS * side / 1000;
	int far_ear = pan > 0 ? 0 : 1; // (a sound on the right: the left ear is the far one)
	for (int ear = 0; ear < 2; ++ear) {
		target_gain[ear] = gain * (ear == far_ear ? drop : 1);
		target_delay[ear] = ear == far_ear ? delay : 0;
		if (at_once) {
			place_gain[ear] = target_gain[ear];
			place_delay[ear] = target_delay[ear];
		}
	}
}

static void place_digi(bool at_once) {
	float pan = 0;
	float gain = 1;
	if (enable_positional_stereo) {
		sound_src_place(&digi_src, &pan, &gain);
	}
	set_digi_place(pan, gain, at_once);
}

// play_digi_sound (the audio stream locked): the new sound's place, at once.
void start_digi_place(void) {
	digi_src = sound_start_src;
	place_digi(true);
}

// Every frame: the screen may have moved on to another room, or the setting changed.
void update_digi_place(void) {
	if (digi_unavailable || digi_audiospec == NULL || !digi_playing) return;
	SDL_LockAudioStream(digi_audio_stream);
	place_digi(false);
	SDL_UnlockAudioStream(digi_audio_stream);
}

// digi_callback: the digitized samples (stereo, both channels the same) placed at each ear.
void place_digi_samples(short* out, const short* in, int frames) {
	int freq = digi_audiospec->freq;
	float glide = 1 - expf(-1000.0f / (GLIDE_MS * freq));
	for (int i = 0; i < frames; ++i) {
		delay_ring[ring_pos % DELAY_RING] = in[2 * i];
		for (int ear = 0; ear < 2; ++ear) {
			place_gain[ear] += (target_gain[ear] - place_gain[ear]) * glide;
			place_delay[ear] += (target_delay[ear] - place_delay[ear]) * glide;
			float lag = MIN(place_delay[ear] * freq, DELAY_RING - 2);
			int whole = (int)lag;
			float fraction = lag - whole;
			float a = (float)delay_ring[(ring_pos - whole) % DELAY_RING];
			float b = (float)delay_ring[(ring_pos - whole - 1) % DELAY_RING];
			int sample = (int)lrintf((a + (b - a) * fraction) * place_gain[ear]);
			out[2 * i + ear] = (short)MAX(-32768, MIN(32767, sample));
		}
		++ring_pos;
	}
}

#endif // USE_POSITIONAL_STEREO
