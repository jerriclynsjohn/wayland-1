/*
 * Copyright © 2013 Marc Chalain
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <cairo.h>
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>
#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell.h"

static char *key_background_image = DATADIR "/weston/pattern.png";
static char *key_background_type = "tile";
static uint32_t key_background_color = 0xff002244;
static void background_section_done(void *data);

static const struct config_key background_config_keys[] = {
	{ "image", CONFIG_KEY_STRING, &key_background_image },
	{ "type", CONFIG_KEY_STRING, &key_background_type },
	{ "color", CONFIG_KEY_UNSIGNED_INTEGER, &key_background_color },
};

static const struct config_section config_sections[] = {
	{ "background",
	  background_config_keys, ARRAY_LENGTH(background_config_keys),
	  background_section_done },
};

struct background {
	struct surface base;
	struct window *window;
	struct widget *widget;
	char *image;
	uint32_t color;
	int painted:1;
	int type:4;
};

enum {
	BACKGROUND_SCALE,
	BACKGROUND_SCALE_CROP,
	BACKGROUND_TILE
};

static void
set_hex_color(cairo_t *cr, uint32_t color)
{
	cairo_set_source_rgba(cr, 
			      ((color >> 16) & 0xff) / 255.0,
			      ((color >>  8) & 0xff) / 255.0,
			      ((color >>  0) & 0xff) / 255.0,
			      ((color >> 24) & 0xff) / 255.0);
}

static void
background_draw(struct widget *widget, void *data)
{
	struct background *background = data;
	cairo_surface_t *surface, *image;
	cairo_pattern_t *pattern;
	cairo_matrix_t matrix;
	cairo_t *cr;
	double im_w, im_h;
	double sx, sy, s;
	double tx, ty;
	struct rectangle allocation;
	struct display *display;
	struct wl_region *opaque;

	surface = window_get_surface(background->window);

	cr = widget_cairo_create(background->widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.2, 1.0);
	cairo_paint(cr);

	widget_get_allocation(widget, &allocation);
	image = NULL;
	if (background->image)
		image = load_cairo_surface(background->image);

	if (image && background->type != -1) {
		im_w = cairo_image_surface_get_width(image);
		im_h = cairo_image_surface_get_height(image);
		sx = im_w / allocation.width;
		sy = im_h / allocation.height;

		pattern = cairo_pattern_create_for_surface(image);

		switch (background->type) {
		case BACKGROUND_SCALE:
			cairo_matrix_init_scale(&matrix, sx, sy);
			cairo_pattern_set_matrix(pattern, &matrix);
			break;
		case BACKGROUND_SCALE_CROP:
			s = (sx < sy) ? sx : sy;
			/* align center */
			tx = (im_w - s * allocation.width) * 0.5;
			ty = (im_h - s * allocation.height) * 0.5;
			cairo_matrix_init_translate(&matrix, tx, ty);
			cairo_matrix_scale(&matrix, s, s);
			cairo_pattern_set_matrix(pattern, &matrix);
			break;
		case BACKGROUND_TILE:
			cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
			break;
		}

		cairo_set_source(cr, pattern);
		cairo_pattern_destroy (pattern);
		cairo_surface_destroy(image);
	} else {
		set_hex_color(cr, background->color);
	}

	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	display = window_get_display(background->window);
	opaque = wl_compositor_create_region(display_get_compositor(display));
	wl_region_add(opaque, allocation.x, allocation.y,
		      allocation.width, allocation.height);
	wl_surface_set_opaque_region(window_get_wl_surface(background->window), opaque);
	wl_region_destroy(opaque);

	background->painted = 1;
	check_desktop_ready(background->window);
}

static void
background_configure(void *data,
		     struct desktop_shell *desktop_shell,
		     uint32_t edges, struct window *window,
		     int32_t width, int32_t height)
{
	struct background *background =
		(struct background *) window_get_user_data(window);

	widget_schedule_resize(background->widget, width, height);
}

static void
background_section_done(void *data)
{
	struct background *background = data;

	if (strcmp(key_background_type, "scale") == 0)
		background->type = BACKGROUND_SCALE;
	else if (strcmp(key_background_type, "scale-crop") == 0)
		background->type = BACKGROUND_SCALE_CROP;
	else if (strcmp(key_background_type, "tile") == 0)
		background->type = BACKGROUND_TILE;
	else
		fprintf(stderr, "invalid background-type: %s\n",
			key_background_type);

	background->image = key_background_image;
	background->color = key_background_color;
	free(key_background_type);
	key_background_type = NULL;
}

void
background_destroy(struct background *background)
{
	widget_destroy(background->widget);
	window_destroy(background->window);

	free(background);
}

struct background *
background_create(struct desktop *desktop, int id)
{
	struct background *background;

	background = malloc(sizeof *background);
	memset(background, 0, sizeof *background);

	background->base.configure = background_configure;
	background->window = window_create_custom(desktop_display(desktop));
	background->widget = window_add_widget(background->window, background);
	window_set_user_data(background->window, background);
	widget_set_redraw_handler(background->widget, background_draw);

	return background;
}

int
background_read_config(struct background *background, int config_fd)
{
	int ret;
	ret = parse_config_file(config_fd,
				config_sections, ARRAY_LENGTH(config_sections),
				&background);
	return ret;
}

int
background_painted(struct background *background)
{
	return background->painted;
}

struct window *
background_window(struct background *background)
{
	return background->window;
}
