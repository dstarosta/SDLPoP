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
#define MAC_MAX_RESOURCE 1200 // the font uses ids 1032..1131

#define PROP_MAC_IMAGE "sdlpop.mac.image"
#define PROP_MAC_ORIGIN_X "sdlpop.mac.origin_x"
#define PROP_MAC_ORIGIN_Y "sdlpop.mac.origin_y"
#define PROP_MAC_HAS_ORIGIN "sdlpop.mac.has_origin"
#define PROP_MAC_DOS_W "sdlpop.mac.dos_w"
#define PROP_MAC_DOS_H "sdlpop.mac.dos_h"

#define MAC_RESOURCES_XML "data/mac/resources.xml"

// One <item> of resources.xml: the image file, relative to data/mac, and its origin.
typedef struct mac_origin_type {
	char* path;
	short x, y;
} mac_origin_type;

// The Mac images of one DAT file: index 0 is the game's, index 1 the loaded mod's.
typedef struct mac_folder_type {
	char name[16];
	bool has_mac_set; // listed in resources.xml
	mac_origin_type origins[2][MAC_MAX_RESOURCE];
} mac_folder_type;

extern dat_type* dat_chain_ptr;

static bool mac_graphics = false;
static bool snes_graphics = false;
static bool snes_paused = false;
static mac_folder_type* mac_folders[MAC_MAX_FOLDERS];
static int mac_folder_count = 0;
static bool mac_index_loaded = false;

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
	apply_graphics_setting();
}

// The path of a data file inside the loaded mod; false without a mod.
static bool get_mod_path(const char* path, char* mod_path, size_t size) {
	if (!use_custom_levelset || skip_mod_data_files || mod_data_path[0] == '\0') return false;
	snprintf_check(mod_path, size, "%s/%s", mod_data_path, path);
	return true;
}

static bool mod_file_exists(const char* path) {
	char mod_path[POP_MAX_PATH];
	return get_mod_path(path, mod_path, sizeof(mod_path)) && file_exists(locate_file(mod_path));
}

// A mod with its own sprites or tiles, as DAT files or data folders.
static bool mod_has_own_graphics(void) {
	static const char* names[] = {"KID", "GUARD", "GUARD1", "GUARD2", "FAT", "SKEL", "VIZIER", "SHADOW", "VDUNGEON", "VPALACE"};
	char path[POP_MAX_PATH];
	for (int i = 0; i < COUNT(names); ++i) {
		snprintf_check(path, sizeof(path), "%s.DAT", names[i]);
		if (!always_use_original_graphics && mod_file_exists(path)) return true;
		snprintf_check(path, sizeof(path), "data/%s", names[i]);
		if (mod_file_exists(path)) return true;
	}
	return false;
}

// setting: 1 = DOS, 2 = Mac, 3 = SNES (as in graphics_setting). A mod can bring its own data/mac or data/snes.
bool is_graphics_available(byte setting) {
	if (setting == 2) return mod_file_exists(MAC_RESOURCES_XML) || file_exists(locate_file(MAC_RESOURCES_XML));
	if (setting == 3) return mod_file_exists("data/snes") || file_exists(locate_file("data/snes"));
	return true;
}

static void free_mac_folders(void) {
	for (int i = 0; i < mac_folder_count; ++i) {
		for (int source = 0; source < 2; ++source) {
			for (int id = 0; id < MAC_MAX_RESOURCE; ++id) {
				SDL_free(mac_folders[i]->origins[source][id].path);
			}
		}
		free(mac_folders[i]);
		mac_folders[i] = NULL;
	}
	mac_folder_count = 0;
	mac_index_loaded = false;
}

void apply_graphics_setting(void) {
	// Auto: a mod with its own sprites or tiles but no Mac set plays with DOS graphics.
	bool auto_mac = mod_file_exists(MAC_RESOURCES_XML) || !mod_has_own_graphics();
	mac_graphics = is_graphics_available(2) && (graphics_setting == 2 || (graphics_setting == 0 && auto_mac));
	snes_graphics = is_graphics_available(3) && graphics_setting == 3;
	free_mac_folders();
}

// After the mod is loaded at startup, and when the splash screen switches the graphics.
void reapply_graphics_setting(void) {
	if (graphics_setting == 2 && !is_graphics_available(2)) {
		printf("Macintosh graphics were requested, but data/mac is missing. Using DOS graphics.\n");
	}
	if (graphics_setting == 3 && !is_graphics_available(3)) {
		printf("SNES graphics were requested, but data/snes is missing. Using DOS graphics.\n");
	}
	bool was_mac = mac_graphics;
	apply_graphics_setting();
	if (mac_graphics != was_mac) {
		recreate_render_surfaces();
#ifdef USE_TEXT
		reload_font();
#endif
	}
}

// A replay can load another mod. The render scale stays, so only the cached Mac folders are dropped.
void mac_graphics_mod_changed(void) {
	free_mac_folders();
}

bool uses_mac_graphics(void) {
	return mac_graphics;
}

bool uses_snes_graphics(void) {
	return snes_graphics;
}

// The folder of a DAT file's loose resources: KID.DAT -> KID.
void dat_folder_name(const dat_type* dat, char* folder, size_t size) {
	snprintf_check(folder, size, "%s", dat->filename);
	size_t len = strlen(folder);
	if (len >= 5 && folder[len - 4] == '.') {
		folder[len - 4] = '\0';
	}
}

// data/snes holds only what differs from DOS, in the same folders as data/; it is looked at before the DAT files.
// Order: the mod's data/snes, the mod's own DOS resource (then NULL), the game's data/snes.
FILE* open_snes_resource(const dat_type* dat, int resource_id, const char* extension) {
	if (!snes_graphics || snes_paused) return NULL;
	char folder[POP_MAX_PATH];
	dat_folder_name(dat, folder, sizeof(folder));
	char path[POP_MAX_PATH];
	snprintf_check(path, sizeof(path), "data/snes/%s/res%d.%s", folder, resource_id, extension);
	char mod_path[POP_MAX_PATH];
	if (get_mod_path(path, mod_path, sizeof(mod_path))) {
		FILE* fp = fopen(locate_file(mod_path), "rb");
		if (fp != NULL) return fp;
	}
	if (mod_provides_resource(dat, resource_id, extension)) return NULL;
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

static mac_folder_type* find_mac_folder(const char* name, bool create) {
	for (int i = 0; i < mac_folder_count; ++i) {
		if (strcasecmp(mac_folders[i]->name, name) == 0) {
			return mac_folders[i];
		}
	}
	if (!create || mac_folder_count >= MAC_MAX_FOLDERS || strlen(name) >= sizeof(mac_folders[0]->name)) return NULL;
	mac_folder_type* folder = calloc(1, sizeof(mac_folder_type));
	if (folder == NULL) return NULL;
	snprintf_check(folder->name, sizeof(folder->name), "%s", name);
	mac_folders[mac_folder_count++] = folder;
	return folder;
}

// The value of attribute name="..." inside one tag, or false.
static bool get_xml_attribute(const char* tag, const char* tag_end, const char* name, char* value, size_t size) {
	size_t name_length = strlen(name);
	for (const char* p = tag + 1; p + name_length + 2 < tag_end; ++p) {
		if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n') &&
		    strncmp(p, name, name_length) == 0 && p[name_length] == '=' && p[name_length + 1] == '"') {
			const char* start = p + name_length + 2;
			const char* end = memchr(start, '"', tag_end - start);
			if (end == NULL || (size_t)(end - start) >= size) return false;
			memcpy(value, start, end - start);
			value[end - start] = '\0';
			return true;
		}
	}
	return false;
}

// resources.xml lists the Mac images like Princed's resources.xml lists the DOS ones:
// <folder file="kid.dat" path="KID"> <item value="401" path="res401.png" x="12" y="-3"/> </folder>
// Folders can nest: the file is inherited and the paths are joined. x and y are the origin in Mac pixels.
static void load_mac_resources_xml(const char* xml_path, int source) {
	size_t size = 0;
	char* text = SDL_LoadFile(locate_file(xml_path), &size);
	if (text == NULL) return;
	enum { MAX_DEPTH = 8 };
	char files[MAX_DEPTH][POP_MAX_PATH];
	char paths[MAX_DEPTH][POP_MAX_PATH];
	int depth = 0;
	files[0][0] = paths[0][0] = '\0';
	const char* text_end = text + size;
	for (const char* p = memchr(text, '<', size); p != NULL; p = memchr(p, '<', text_end - p)) {
		if (text_end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
			const char* comment_end = SDL_strstr(p, "-->");
			if (comment_end == NULL) break;
			p = comment_end + 3;
			continue;
		}
		const char* tag_end = memchr(p, '>', text_end - p);
		if (tag_end == NULL) break;
		bool self_closing = tag_end[-1] == '/';
		char value[POP_MAX_PATH];
		if (strncmp(p, "</folder", 8) == 0) {
			if (depth > 0) --depth;
		} else if (strncmp(p, "<folder", 7) == 0 && !self_closing && depth + 1 < MAX_DEPTH) {
			++depth;
			if (!get_xml_attribute(p, tag_end, "file", files[depth], sizeof(files[depth]))) {
				memcpy(files[depth], files[depth - 1], sizeof(files[depth]));
			}
			if (!get_xml_attribute(p, tag_end, "path", value, sizeof(value))) {
				memcpy(paths[depth], paths[depth - 1], sizeof(paths[depth]));
			} else if (paths[depth - 1][0] != '\0') {
				char joined[POP_MAX_PATH];
				snprintf_check(joined, sizeof(joined), "%s/%s", paths[depth - 1], value);
				memcpy(paths[depth], joined, sizeof(paths[depth]));
			} else {
				memcpy(paths[depth], value, sizeof(paths[depth]));
			}
		} else if (strncmp(p, "<item", 5) == 0 && files[depth][0] != '\0') {
			char item_path[POP_MAX_PATH];
			char x[16] = "0", y[16] = "0";
			get_xml_attribute(p, tag_end, "x", x, sizeof(x));
			get_xml_attribute(p, tag_end, "y", y, sizeof(y));
			if (get_xml_attribute(p, tag_end, "value", value, sizeof(value)) &&
			    get_xml_attribute(p, tag_end, "path", item_path, sizeof(item_path))) {
				int id = atoi(value);
				// The folder is named after the DAT file without its extension: kid.dat -> KID.
				char folder_name[POP_MAX_PATH];
				snprintf_check(folder_name, sizeof(folder_name), "%s", files[depth]);
				char* dot = strrchr(folder_name, '.');
				if (dot != NULL) *dot = '\0';
				mac_folder_type* folder = find_mac_folder(folder_name, true);
				if (folder != NULL && id >= 0 && id < MAC_MAX_RESOURCE) {
					folder->has_mac_set = true;
					mac_origin_type* origin = &folder->origins[source][id];
					char full_path[POP_MAX_PATH];
					if (paths[depth][0] != '\0') {
						snprintf_check(full_path, sizeof(full_path), "%s/%s", paths[depth], item_path);
					} else {
						snprintf_check(full_path, sizeof(full_path), "%s", item_path);
					}
					SDL_free(origin->path);
					origin->path = SDL_strdup(full_path);
					origin->x = (short)atoi(x);
					origin->y = (short)atoi(y);
				}
			}
		}
		p = tag_end + 1;
	}
	SDL_free(text);
}

static mac_folder_type* get_mac_folder(const char* name) {
	if (!mac_index_loaded) {
		mac_index_loaded = true;
		load_mac_resources_xml(MAC_RESOURCES_XML, 0);
		char mod_path[POP_MAX_PATH];
		if (get_mod_path(MAC_RESOURCES_XML, mod_path, sizeof(mod_path))) {
			load_mac_resources_xml(mod_path, 1);
		}
	}
	return find_mac_folder(name, false);
}

// An image missing from a folder's Mac set is left blank, so the gap shows.
// Folders without a Mac set (GUARD1, a mod's extra DATs), characters the Mac font lacks
// and the DOS credits (TITLE res45) stay DOS.
bool mac_blanks_missing_image(const dat_type* source, int resource_id) {
	if (!mac_graphics || source == NULL) return false;
	char folder_name[POP_MAX_PATH];
	dat_folder_name(source, folder_name, sizeof(folder_name));
	if (strcasecmp(folder_name, "font") == 0) return false;
	if (resource_id == 45 && strcasecmp(folder_name, "TITLE") == 0) return false;
	mac_folder_type* folder = get_mac_folder(folder_name);
	return folder != NULL && folder->has_mac_set;
}

bool mac_has_image(const char* folder_name, int resource_id) {
	if (!mac_graphics || resource_id < 0 || resource_id >= MAC_MAX_RESOURCE) return false;
	mac_folder_type* folder = get_mac_folder(folder_name);
	return folder != NULL && (folder->origins[0][resource_id].path != NULL || folder->origins[1][resource_id].path != NULL);
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
	// High-res art wins: the mod's Mac image, then the game's; DOS art only when neither loads.
	for (dat_type* dat = dat_chain_ptr; dat != NULL; dat = dat->next_dat) {
		char folder_name[POP_MAX_PATH];
		dat_folder_name(dat, folder_name, sizeof(folder_name));
		mac_folder_type* folder = get_mac_folder(folder_name);
		if (folder == NULL) continue;
		for (int source = 1; source >= 0; --source) {
			const mac_origin_type* origin = &folder->origins[source][resource_id];
			if (origin->path == NULL) continue;
			char path[POP_MAX_PATH];
			snprintf_check(path, sizeof(path), "data/mac/%s", origin->path);
			char mod_path[POP_MAX_PATH];
			const char* image_path = path;
			if (source == 1) {
				if (!get_mod_path(path, mod_path, sizeof(mod_path))) continue;
				image_path = mod_path;
			}
			size_t size = 0;
			void* data = SDL_LoadFile(locate_file(image_path), &size);
			if (data == NULL) continue;
			image_type* image = decode_png(data, (int)size);
			SDL_free(data);
			if (image == NULL) continue;
			set_mac_image_properties(image, true, origin->x, origin->y);
			return image;
		}
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

// A recorded size of 0 means the DOS data has no such image: DOS collisions then see 0 too.
int dos_image_width(const image_type* image) {
	SDL_PropertiesID props = SDL_GetSurfaceProperties((image_type*)image);
	if (SDL_HasProperty(props, PROP_MAC_DOS_W)) return (int)SDL_GetNumberProperty(props, PROP_MAC_DOS_W, 0);
	return logical_image_width(image);
}

int dos_image_height(const image_type* image) {
	SDL_PropertiesID props = SDL_GetSurfaceProperties((image_type*)image);
	if (SDL_HasProperty(props, PROP_MAC_DOS_H)) return (int)SDL_GetNumberProperty(props, PROP_MAC_DOS_H, 0);
	return logical_image_height(image);
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
