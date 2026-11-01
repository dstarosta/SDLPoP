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

// Macintosh high-resolution graphics: loading data/mac and drawing at 640x400.
// The native-origin compositor is based on Prince of Persia Remastered by The-Alok (GPL-3.0-or-later).

#include "common.h"

#define MAC_SCALE 2
#define MAC_MAX_FOLDERS 16
#define MAC_MAX_RESOURCE 1000

#define PROP_MAC_IMAGE "sdlpop.mac.image"
#define PROP_MAC_ORIGIN_X "sdlpop.mac.origin_x"
#define PROP_MAC_ORIGIN_Y "sdlpop.mac.origin_y"
#define PROP_MAC_HAS_ORIGIN "sdlpop.mac.has_origin"
#define PROP_MAC_DOS_W "sdlpop.mac.dos_w"
#define PROP_MAC_DOS_H "sdlpop.mac.dos_h"

typedef struct mac_folder_type {
	char name[16];
	bool present[MAC_MAX_RESOURCE];
	short origin_x[MAC_MAX_RESOURCE];
	short origin_y[MAC_MAX_RESOURCE];
} mac_folder_type;

extern dat_type* dat_chain_ptr;

static bool mac_graphics = false;
static bool snes_graphics = false;
static bool snes_paused = false;
static mac_folder_type* mac_folders[MAC_MAX_FOLDERS];
static int mac_folder_count = 0;

void init_mac_graphics(void) {
	const char* param = check_param("graphics=");
	if (param != NULL) {
		static const char* names[] = {"auto", "dos", "mac", "snes"};
		for (int i = 0; i < COUNT(names); ++i) {
			if (strcasecmp(param + strlen("graphics="), names[i]) == 0) {
				graphics_setting = (byte)i;
			}
		}
	}
	if (graphics_setting == 2 && !is_graphics_available(2)) {
		printf("Macintosh graphics were requested, but data/mac is missing. Using DOS graphics.\n");
	}
	if (graphics_setting == 3 && !is_graphics_available(3)) {
		printf("SNES graphics were requested, but data/snes is missing. Using DOS graphics.\n");
	}
	apply_graphics_setting();
}

// setting: 1 = DOS, 2 = Mac, 3 = SNES (as in graphics_setting).
bool is_graphics_available(byte setting) {
	if (setting == 2) return file_exists(locate_file("data/mac/KID/origins.txt"));
	if (setting == 3) return file_exists(locate_file("data/snes"));
	return true;
}

// At startup, and when the splash screen switches the graphics (then followed by recreate_render_surfaces()).
void apply_graphics_setting(void) {
	mac_graphics = is_graphics_available(2) && (graphics_setting == 0 || graphics_setting == 2);
	snes_graphics = is_graphics_available(3) && graphics_setting == 3;
}

bool uses_mac_graphics(void) {
	return mac_graphics;
}

bool uses_snes_graphics(void) {
	return snes_graphics;
}

// data/snes holds only what differs from DOS, in the same folders as data/; it is looked at before the DAT files.
FILE* open_snes_resource(const char* dat_filename, int resource_id, const char* extension) {
	if (!snes_graphics || snes_paused) return NULL;
	char folder[POP_MAX_PATH];
	snprintf_check(folder, sizeof(folder), "%s", dat_filename);
	size_t len = strlen(folder);
	if (len >= 5 && folder[len - 4] == '.') {
		folder[len - 4] = '\0';
	}
	char path[POP_MAX_PATH];
	snprintf_check(path, sizeof(path), "data/snes/%s/res%d.%s", folder, resource_id, extension);
	return fopen(locate_file(path), "rb");
}

// While paused, resources load from the DOS data as if data/snes did not exist.
void pause_snes_resources(bool paused) {
	snes_paused = paused;
}

int get_render_scale(void) {
	return mac_graphics ? MAC_SCALE : 1;
}

int get_render_width(void) {
	return 320 * get_render_scale();
}

int get_render_height(void) {
	return 200 * get_render_scale();
}

static mac_folder_type* get_mac_folder(const char* name) {
	for (int i = 0; i < mac_folder_count; ++i) {
		if (strcasecmp(mac_folders[i]->name, name) == 0) {
			return mac_folders[i];
		}
	}
	if (mac_folder_count >= MAC_MAX_FOLDERS) return NULL;
	mac_folder_type* folder = calloc(1, sizeof(mac_folder_type));
	if (folder == NULL) return NULL;
	snprintf_check(folder->name, sizeof(folder->name), "%s", name);
	char path[POP_MAX_PATH];
	snprintf_check(path, sizeof(path), "data/mac/%s/origins.txt", name);
	FILE* fp = fopen(locate_file(path), "r");
	if (fp != NULL) {
		int id, x, y;
		while (fscanf(fp, "%d %d %d", &id, &x, &y) == 3) {
			if (id >= 0 && id < MAC_MAX_RESOURCE) {
				folder->present[id] = true;
				folder->origin_x[id] = (short)x;
				folder->origin_y[id] = (short)y;
			}
		}
		fclose(fp);
	}
	mac_folders[mac_folder_count++] = folder;
	return folder;
}

static void dat_folder_name(const dat_type* dat, char* folder, size_t size) {
	snprintf_check(folder, size, "%s", dat->filename);
	size_t len = strlen(folder);
	if (len >= 5 && folder[len - 4] == '.') {
		folder[len - 4] = '\0';
	}
}

bool mac_has_image(const char* folder_name, int resource_id) {
	if (!mac_graphics || resource_id < 0 || resource_id >= MAC_MAX_RESOURCE) return false;
	mac_folder_type* folder = get_mac_folder(folder_name);
	return folder != NULL && folder->present[resource_id];
}

static void set_mac_image_properties(image_type* image, bool has_origin, short origin_x, short origin_y) {
	SDL_PropertiesID props = SDL_GetSurfaceProperties(image);
	SDL_SetBooleanProperty(props, PROP_MAC_IMAGE, true);
	SDL_SetBooleanProperty(props, PROP_MAC_HAS_ORIGIN, has_origin);
	SDL_SetNumberProperty(props, PROP_MAC_ORIGIN_X, origin_x);
	SDL_SetNumberProperty(props, PROP_MAC_ORIGIN_Y, origin_y);
}

image_type* load_mac_image(int resource_id, image_type* (*decode_png)(const void* data, int size)) {
	if (!mac_graphics || resource_id < 0 || resource_id >= MAC_MAX_RESOURCE) return NULL;
	for (dat_type* dat = dat_chain_ptr; dat != NULL; dat = dat->next_dat) {
		char folder_name[POP_MAX_PATH];
		dat_folder_name(dat, folder_name, sizeof(folder_name));
		mac_folder_type* folder = get_mac_folder(folder_name);
		if (folder == NULL || !folder->present[resource_id]) continue;
		char path[POP_MAX_PATH];
		snprintf_check(path, sizeof(path), "data/mac/%s/res%d.png", folder_name, resource_id);
		size_t size = 0;
		void* data = SDL_LoadFile(locate_file(path), &size);
		if (data == NULL) continue;
		image_type* image = decode_png(data, (int)size);
		SDL_free(data);
		if (image == NULL) continue;
		set_mac_image_properties(image, true, folder->origin_x[resource_id], folder->origin_y[resource_id]);
		return image;
	}
	return NULL;
}

void set_mac_image_dos_size(image_type* image, int width, int height) {
	SDL_PropertiesID props = SDL_GetSurfaceProperties(image);
	SDL_SetNumberProperty(props, PROP_MAC_DOS_W, width);
	SDL_SetNumberProperty(props, PROP_MAC_DOS_H, height);
}

bool is_native_resolution_image(const image_type* image) {
	if (!mac_graphics || image == NULL) return false;
	return SDL_GetBooleanProperty(SDL_GetSurfaceProperties((image_type*)image), PROP_MAC_IMAGE, false);
}

void mark_native_resolution_image(image_type* image) {
	if (mac_graphics && image != NULL) {
		set_mac_image_properties(image, false, 0, 0);
	}
}

void copy_native_image_properties(image_type* from, image_type* to) {
	if (is_native_resolution_image(from) && to != NULL) {
		SDL_CopyProperties(SDL_GetSurfaceProperties(from), SDL_GetSurfaceProperties(to));
	}
}

bool get_native_image_origin(const image_type* image, short* origin_x, short* origin_y) {
	if (!is_native_resolution_image(image)) return false;
	SDL_PropertiesID props = SDL_GetSurfaceProperties((image_type*)image);
	if (!SDL_GetBooleanProperty(props, PROP_MAC_HAS_ORIGIN, false)) return false;
	if (origin_x) *origin_x = (short)SDL_GetNumberProperty(props, PROP_MAC_ORIGIN_X, 0);
	if (origin_y) *origin_y = (short)SDL_GetNumberProperty(props, PROP_MAC_ORIGIN_Y, 0);
	return true;
}

int logical_image_width(const image_type* image) {
	if (image == NULL) return 0;
	return is_native_resolution_image(image) ? (image->w + MAC_SCALE - 1) / MAC_SCALE : image->w;
}

int logical_image_height(const image_type* image) {
	if (image == NULL) return 0;
	return is_native_resolution_image(image) ? (image->h + MAC_SCALE - 1) / MAC_SCALE : image->h;
}

int dos_image_width(const image_type* image) {
	int width = (int)SDL_GetNumberProperty(SDL_GetSurfaceProperties((image_type*)image), PROP_MAC_DOS_W, 0);
	return width > 0 ? width : logical_image_width(image);
}

int dos_image_height(const image_type* image) {
	int height = (int)SDL_GetNumberProperty(SDL_GetSurfaceProperties((image_type*)image), PROP_MAC_DOS_H, 0);
	return height > 0 ? height : logical_image_height(image);
}

// Indexed to ARGB by hand: SDL3 cannot always convert INDEX8 surfaces. Index 0 becomes transparent.
SDL_Surface* convert_image_to_argb(image_type* image) {
	if (!SDL_ISPIXELFORMAT_INDEXED(image->format)) {
		return SDL_ConvertSurface(image, SDL_PIXELFORMAT_ARGB8888);
	}
	SDL_Palette* palette = SDL_GetSurfacePalette(image);
	SDL_Surface* result = SDL_CreateSurface(image->w, image->h, SDL_PIXELFORMAT_ARGB8888);
	if (result == NULL || palette == NULL) {
		SDL_DestroySurface(result);
		return NULL;
	}
	Uint32 colors[256] = {0};
	for (int i = 1; i < palette->ncolors && i < 256; ++i) {
		SDL_Color c = palette->colors[i];
		colors[i] = 0xFF000000u | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | c.b;
	}
	if (!SDL_LockSurface(image)) {
		SDL_DestroySurface(result);
		return NULL;
	}
	if (!SDL_LockSurface(result)) {
		SDL_UnlockSurface(image);
		SDL_DestroySurface(result);
		return NULL;
	}
	for (int y = 0; y < image->h; ++y) {
		const byte* src = (const byte*)image->pixels + y * image->pitch;
		Uint32* dst = (Uint32*)((byte*)result->pixels + y * result->pitch);
		for (int x = 0; x < image->w; ++x) {
			dst[x] = colors[src[x]];
		}
	}
	SDL_UnlockSurface(result);
	SDL_UnlockSurface(image);
	return result;
}

static void add_physical_drect(int x, int y, int w, int h) {
	rect_type rect;
	rect.left = x >= 0 ? x / MAC_SCALE : -((-x + MAC_SCALE - 1) / MAC_SCALE);
	rect.top = y >= 0 ? y / MAC_SCALE : -((-y + MAC_SCALE - 1) / MAC_SCALE);
	rect.right = (x + w + MAC_SCALE - 1) / MAC_SCALE;
	rect.bottom = (y + h + MAC_SCALE - 1) / MAC_SCALE;
	add_drect(&rect);
}

// Draws an image at logical coordinates: DOS images are scaled up, Mac images are copied 1:1.
void blit_image_scaled_for_renderer(image_type* image, const SDL_Rect* src_rect, SDL_Surface* target, int logical_x, int logical_y) {
	int scale = get_render_scale();
	SDL_Rect dest_rect = {logical_x * scale, logical_y * scale, 0, 0};
	int w = src_rect ? src_rect->w : image->w;
	int h = src_rect ? src_rect->h : image->h;
	if (scale == 1 || is_native_resolution_image(image)) {
		dest_rect.w = w;
		dest_rect.h = h;
		if (!SDL_BlitSurface(image, src_rect, target, &dest_rect)) {
			sdlperror("blit_image_scaled_for_renderer: SDL_BlitSurface");
		}
		return;
	}
	dest_rect.w = w * scale;
	dest_rect.h = h * scale;
	if (SDL_BlitSurfaceScaled(image, src_rect, target, &dest_rect, SDL_SCALEMODE_NEAREST)) return;
	// Some indexed blits cannot be scaled directly; convert this one blit and keep the colour key.
	SDL_Surface* converted = convert_image_to_argb(image);
	if (converted == NULL) {
		sdlperror("blit_image_scaled_for_renderer: convert");
		return;
	}
	bool keyed = SDL_SurfaceHasColorKey(image);
	if (!keyed && SDL_ISPIXELFORMAT_INDEXED(image->format)) {
		// Opaque blit: index 0 must stay visible.
		SDL_Palette* palette = SDL_GetSurfacePalette(image);
		if (palette != NULL && palette->ncolors > 0) {
			SDL_Color c = palette->colors[0];
			Uint32 zero = 0xFF000000u | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | c.b;
			if (SDL_LockSurface(converted)) {
				for (int y = 0; y < image->h; ++y) {
					const byte* src = (const byte*)image->pixels + y * image->pitch;
					Uint32* dst = (Uint32*)((byte*)converted->pixels + y * converted->pitch);
					for (int x = 0; x < image->w; ++x) {
						if (src[x] == 0) dst[x] = zero;
					}
				}
				SDL_UnlockSurface(converted);
			}
		}
	}
	SDL_SetSurfaceBlendMode(converted, keyed ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	if (!SDL_BlitSurfaceScaled(converted, src_rect, target, &dest_rect, SDL_SCALEMODE_NEAREST)) {
		sdlperror("blit_image_scaled_for_renderer: SDL_BlitSurfaceScaled");
	}
	SDL_DestroySurface(converted);
}

void draw_native_rect_physical(int x, int y, int w, int h, Uint32 rgb) {
	SDL_Rect rect = {x, y, w, h};
	SDL_FillSurfaceRect(current_target_surface, &rect,
	                    SDL_MapSurfaceRGB(current_target_surface, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb));
	if (need_drects) {
		SDL_Rect clip, visible;
		SDL_GetSurfaceClipRect(current_target_surface, &clip);
		if (SDL_GetRectIntersection(&rect, &clip, &visible)) {
			add_physical_drect(visible.x, visible.y, visible.w, visible.h);
		}
	}
}

void draw_native_image_physical(image_type* image, int physical_x, int physical_y) {
	if (image == NULL) return;
	SDL_Rect src_rect = {0, 0, image->w, image->h};
	SDL_Rect dst_rect = {physical_x, physical_y, image->w, image->h};
	if (SDL_ISPIXELFORMAT_INDEXED(image->format)) {
		SDL_SetSurfaceBlendMode(image, SDL_BLENDMODE_NONE);
		SDL_SetSurfaceColorKey(image, true, 0);
	} else {
		SDL_SetSurfaceBlendMode(image, SDL_BLENDMODE_BLEND);
	}
	SDL_SetSurfaceAlphaMod(image, 255);
	if (!SDL_BlitSurface(image, &src_rect, current_target_surface, &dst_rect)) {
		sdlperror("draw_native_image_physical: SDL_BlitSurface");
	}
	if (need_drects) {
		add_physical_drect(physical_x, physical_y, image->w, image->h);
	}
}

// XOR in physical pixels, skipping transparent pixels.
static void draw_native_xor(image_type* image, int physical_x, int physical_y) {
	SDL_Rect destination = {physical_x, physical_y, image->w, image->h};
	SDL_Rect clip, visible;
	SDL_GetSurfaceClipRect(current_target_surface, &clip);
	if (!SDL_GetRectIntersection(&destination, &clip, &visible)) return;
	SDL_Surface* source = convert_image_to_argb(image);
	SDL_Surface* result = SDL_CreateSurface(visible.w, visible.h, SDL_PIXELFORMAT_ARGB8888);
	if (source == NULL || result == NULL) {
		sdlperror("draw_native_xor: allocate surfaces");
		SDL_DestroySurface(source);
		SDL_DestroySurface(result);
		return;
	}
	SDL_BlendMode saved_blend;
	SDL_GetSurfaceBlendMode(current_target_surface, &saved_blend);
	SDL_SetSurfaceBlendMode(current_target_surface, SDL_BLENDMODE_NONE);
	SDL_Rect local = {0, 0, visible.w, visible.h};
	SDL_BlitSurface(current_target_surface, &visible, result, &local);
	SDL_SetSurfaceBlendMode(current_target_surface, saved_blend);
	if (SDL_LockSurface(source) && SDL_LockSurface(result)) {
		for (int y = 0; y < visible.h; ++y) {
			Uint32* src = (Uint32*)((byte*)source->pixels + (visible.y - physical_y + y) * source->pitch);
			Uint32* dst = (Uint32*)((byte*)result->pixels + y * result->pitch);
			for (int x = 0; x < visible.w; ++x) {
				Uint32 color = src[visible.x - physical_x + x];
				if (color & 0xFF000000) dst[x] ^= color & 0xFFFFFF;
			}
		}
		SDL_UnlockSurface(source);
		SDL_UnlockSurface(result);
		SDL_SetSurfaceBlendMode(result, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(result, NULL, current_target_surface, &visible);
	}
	SDL_DestroySurface(source);
	SDL_DestroySurface(result);
	if (need_drects) {
		add_physical_drect(visible.x, visible.y, visible.w, visible.h);
	}
}

void draw_native_image_physical_blit(image_type* image, int physical_x, int physical_y, int blit) {
	if (image == NULL) return;
	if (blit == blitters_3_xor) {
		draw_native_xor(image, physical_x, physical_y);
		return;
	}
	if (blit != blitters_9_black) {
		draw_native_image_physical(image, physical_x, physical_y);
		return;
	}
	// The black blitter is an occlusion mask: keep the alpha, paint every opaque pixel black.
	SDL_Surface* black_mask = convert_image_to_argb(image);
	if (black_mask == NULL) {
		sdlperror("draw_native_image_physical_blit: convert");
		return;
	}
	if (SDL_LockSurface(black_mask)) {
		for (int y = 0; y < black_mask->h; ++y) {
			Uint32* pixel = (Uint32*)((byte*)black_mask->pixels + y * black_mask->pitch);
			for (int x = 0; x < black_mask->w; ++x) {
				pixel[x] &= 0xFF000000;
			}
		}
		SDL_UnlockSurface(black_mask);
	}
	draw_native_image_physical(black_mask, physical_x, physical_y);
	SDL_DestroySurface(black_mask);
}
