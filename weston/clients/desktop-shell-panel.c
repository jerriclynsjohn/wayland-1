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
#include <sys/timerfd.h>
#include <sys/epoll.h> 
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>
#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell.h"

extern char **environ; /* defined by libc */

struct panel {
	struct surface base;
	struct window *window;
	struct widget *widget;
	struct wl_list launcher_list;
	struct panel_clock *clock;
	int painted;
};

struct panel_launcher {
	struct widget *widget;
	struct panel *panel;
	cairo_surface_t *icon;
	int focused, pressed;
	char *path;
	struct wl_list link;
	struct wl_array envp;
	struct wl_array argv;
};

struct panel_clock {
	struct widget *widget;
	struct panel *panel;
	struct task clock_task;
	int clock_fd;
};

static uint32_t key_panel_color = 0xaa000000;
static char *key_launcher_icon;
static char *key_launcher_path;
static void launcher_section_done(void *data);

static const struct config_key panel_config_keys[] = {
	{ "color", CONFIG_KEY_UNSIGNED_INTEGER, &key_panel_color },
};

static const struct config_key launcher_config_keys[] = {
	{ "icon", CONFIG_KEY_STRING, &key_launcher_icon },
	{ "path", CONFIG_KEY_STRING, &key_launcher_path },
};

static const struct config_section config_sections[] = {
	{ "panel",
	  panel_config_keys, ARRAY_LENGTH(panel_config_keys) },
	{ "launcher",
	  launcher_config_keys, ARRAY_LENGTH(launcher_config_keys),
	  launcher_section_done }
};

static void
panel_launcher_activate(struct panel_launcher *widget)
{
	char **argv;
	pid_t pid;

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork failed: %m\n");
		return;
	}

	if (pid)
		return;

	argv = widget->argv.data;
	if (execve(argv[0], argv, widget->envp.data) < 0) {
		fprintf(stderr, "execl '%s' failed: %m\n", argv[0]);
		exit(1);
	}
}

static void
panel_launcher_redraw_handler(struct widget *widget, void *data)
{
	struct panel_launcher *launcher = data;
	struct rectangle allocation;
	cairo_t *cr;

	cr = widget_cairo_create(launcher->panel->widget);

	widget_get_allocation(widget, &allocation);
	if (launcher->pressed) {
		allocation.x++;
		allocation.y++;
	}

	cairo_set_source_surface(cr, launcher->icon,
				 allocation.x, allocation.y);
	cairo_paint(cr);

	if (launcher->focused) {
		cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.4);
		cairo_mask_surface(cr, launcher->icon,
				   allocation.x, allocation.y);
	}

	cairo_destroy(cr);
}

static int
panel_launcher_motion_handler(struct widget *widget, struct input *input,
			      uint32_t time, float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	widget_set_tooltip(widget, basename((char *)launcher->path), x, y);

	return CURSOR_LEFT_PTR;
}

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
panel_redraw_handler(struct widget *widget, void *data)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	struct panel *panel = data;

	cr = widget_cairo_create(panel->widget);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_hex_color(cr, key_panel_color);
	cairo_paint(cr);

	cairo_destroy(cr);
	surface = window_get_surface(panel->window);
	cairo_surface_destroy(surface);
	panel->painted = 1;
	check_desktop_ready(panel->window);
}

static int
panel_launcher_enter_handler(struct widget *widget, struct input *input,
			     float x, float y, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 1;
	widget_schedule_redraw(widget);

	return CURSOR_LEFT_PTR;
}

static void
panel_launcher_leave_handler(struct widget *widget,
			     struct input *input, void *data)
{
	struct panel_launcher *launcher = data;

	launcher->focused = 0;
	widget_destroy_tooltip(widget);
	widget_schedule_redraw(widget);
}

static void
panel_launcher_button_handler(struct widget *widget,
			      struct input *input, uint32_t time,
			      uint32_t button,
			      enum wl_pointer_button_state state, void *data)
{
	struct panel_launcher *launcher;

	launcher = widget_get_user_data(widget);
	widget_schedule_redraw(widget);
	if (state == WL_POINTER_BUTTON_STATE_RELEASED)
		panel_launcher_activate(launcher);
}

static void
clock_func(struct task *task, uint32_t events)
{
	struct panel_clock *clock =
		container_of(task, struct panel_clock, clock_task);
	uint64_t exp;

	if (read(clock->clock_fd, &exp, sizeof exp) != sizeof exp)
		abort();
	widget_schedule_redraw(clock->widget);
}

static void
panel_clock_redraw_handler(struct widget *widget, void *data)
{
	struct panel_clock *clock = data;
	cairo_t *cr;
	struct rectangle allocation;
	cairo_text_extents_t extents;
	cairo_font_extents_t font_extents;
	time_t rawtime;
	struct tm * timeinfo;
	char string[128];

	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(string, sizeof string, "%a %b %d, %I:%M %p", timeinfo);

	widget_get_allocation(widget, &allocation);
	if (allocation.width == 0)
		return;

	cr = widget_cairo_create(clock->panel->widget);
	cairo_select_font_face(cr, "sans",
			       CAIRO_FONT_SLANT_NORMAL,
			       CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);
	cairo_text_extents(cr, string, &extents);
	cairo_font_extents (cr, &font_extents);
	cairo_move_to(cr, allocation.x + 5,
		      allocation.y + 3 * (allocation.height >> 2) + 1);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_show_text(cr, string);
	cairo_move_to(cr, allocation.x + 4,
		      allocation.y + 3 * (allocation.height >> 2));
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_show_text(cr, string);
	cairo_destroy(cr);
}

static int
clock_timer_reset(struct panel_clock *clock)
{
	struct itimerspec its;

	its.it_interval.tv_sec = 60;
	its.it_interval.tv_nsec = 0;
	its.it_value.tv_sec = 60;
	its.it_value.tv_nsec = 0;
	if (timerfd_settime(clock->clock_fd, 0, &its, NULL) < 0) {
		fprintf(stderr, "could not set timerfd\n: %m");
		return -1;
	}

	return 0;
}

static void
panel_destroy_clock(struct panel_clock *clock)
{
	widget_destroy(clock->widget);

	close(clock->clock_fd);

	free(clock);
}

static void
panel_add_clock(struct panel *panel)
{
	struct panel_clock *clock;
	int timerfd;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (timerfd < 0) {
		fprintf(stderr, "could not create timerfd\n: %m");
		return;
	}

	clock = malloc(sizeof *clock);
	memset(clock, 0, sizeof *clock);
	clock->panel = panel;
	panel->clock = clock;
	clock->clock_fd = timerfd;

	clock->clock_task.run = clock_func;
	display_watch_fd(window_get_display(panel->window), clock->clock_fd,
			 EPOLLIN, &clock->clock_task);
	clock_timer_reset(clock);

	clock->widget = widget_add_widget(panel->widget, clock);
	widget_set_redraw_handler(clock->widget, panel_clock_redraw_handler);
}

static void
menu_func(struct window *window, int index, void *data)
{
	printf("Selected index %d from a panel menu.\n", index);
}

static void
show_menu(struct panel *panel, struct input *input, uint32_t time)
{
	int32_t x, y;
	static const char *entries[] = {
		"Roy", "Pris", "Leon", "Zhora"
	};

	input_get_position(input, &x, &y);
	window_show_menu(window_get_display(panel->window),
			 input, time, panel->window,
			 x - 10, y - 10, menu_func, entries, 4);
}

static void
panel_button_handler(struct widget *widget,
		     struct input *input, uint32_t time,
		     uint32_t button,
		     enum wl_pointer_button_state state, void *data)
{
	struct panel *panel = data;

	if (button == BTN_RIGHT && state == WL_POINTER_BUTTON_STATE_PRESSED)
		show_menu(panel, input, time);
}

static void
panel_resize_handler(struct widget *widget,
		     int32_t width, int32_t height, void *data)
{
	struct panel_launcher *launcher;
	struct panel *panel = data;
	int x, y, w, h;
	
	x = 10;
	y = 16;
	wl_list_for_each(launcher, &panel->launcher_list, link) {
		w = cairo_image_surface_get_width(launcher->icon);
		h = cairo_image_surface_get_height(launcher->icon);
		widget_set_allocation(launcher->widget,
				      x, y - h / 2, w + 1, h + 1);
		x += w + 10;
	}
	h=20;
	w=170;

	if (panel->clock)
		widget_set_allocation(panel->clock->widget,
				      width - w - 8, y - h / 2, w + 1, h + 1);
}

static void
panel_configure(void *data,
		struct desktop_shell *desktop_shell,
		uint32_t edges, struct window *window,
		int32_t width, int32_t height)
{
	struct surface *surface = window_get_user_data(window);
	struct panel *panel = container_of(surface, struct panel, base);

	window_schedule_resize(panel->window, width, 32);
}

static void
panel_destroy_launcher(struct panel_launcher *launcher)
{
	wl_array_release(&launcher->argv);
	wl_array_release(&launcher->envp);

	free(launcher->path);

	cairo_surface_destroy(launcher->icon);

	widget_destroy(launcher->widget);
	wl_list_remove(&launcher->link);

	free(launcher);
}

static cairo_surface_t *
load_icon_or_fallback(const char *icon)
{
	cairo_surface_t *surface = cairo_image_surface_create_from_png(icon);
	cairo_status_t status;
	cairo_t *cr;

	status = cairo_surface_status(surface);
	if (status == CAIRO_STATUS_SUCCESS)
		return surface;

	cairo_surface_destroy(surface);
	fprintf(stderr, "ERROR loading icon from file '%s', error: '%s'\n",
		icon, cairo_status_to_string(status));

	/* draw fallback icon */
	surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
					     20, 20);
	cr = cairo_create(surface);

	cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 1);
	cairo_paint(cr);

	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_rectangle(cr, 0, 0, 20, 20);
	cairo_move_to(cr, 4, 4);
	cairo_line_to(cr, 16, 16);
	cairo_move_to(cr, 4, 16);
	cairo_line_to(cr, 16, 4);
	cairo_stroke(cr);

	cairo_destroy(cr);

	return surface;
}

static void
panel_add_launcher(struct panel *panel, const char *icon, const char *path)
{
	struct panel_launcher *launcher;
	char *start, *p, *eq, **ps;
	int i, j, k;

	launcher = malloc(sizeof *launcher);
	memset(launcher, 0, sizeof *launcher);
	launcher->icon = load_icon_or_fallback(icon);
	launcher->path = strdup(path);

	wl_array_init(&launcher->envp);
	wl_array_init(&launcher->argv);
	for (i = 0; environ[i]; i++) {
		ps = wl_array_add(&launcher->envp, sizeof *ps);
		*ps = environ[i];
	}
	j = 0;

	start = launcher->path;
	while (*start) {
		for (p = start, eq = NULL; *p && !isspace(*p); p++)
			if (*p == '=')
				eq = p;

		if (eq && j == 0) {
			ps = launcher->envp.data;
			for (k = 0; k < i; k++)
				if (strncmp(ps[k], start, eq - start) == 0) {
					ps[k] = start;
					break;
				}
			if (k == i) {
				ps = wl_array_add(&launcher->envp, sizeof *ps);
				*ps = start;
				i++;
			}
		} else {
			ps = wl_array_add(&launcher->argv, sizeof *ps);
			*ps = start;
			j++;
		}

		while (*p && isspace(*p))
			*p++ = '\0';

		start = p;
	}

	ps = wl_array_add(&launcher->envp, sizeof *ps);
	*ps = NULL;
	ps = wl_array_add(&launcher->argv, sizeof *ps);
	*ps = NULL;

	launcher->panel = panel;
	wl_list_insert(panel->launcher_list.prev, &launcher->link);

	launcher->widget = widget_add_widget(panel->widget, launcher);
	widget_set_enter_handler(launcher->widget,
				 panel_launcher_enter_handler);
	widget_set_leave_handler(launcher->widget,
				   panel_launcher_leave_handler);
	widget_set_button_handler(launcher->widget,
				    panel_launcher_button_handler);
	widget_set_redraw_handler(launcher->widget,
				  panel_launcher_redraw_handler);
	widget_set_motion_handler(launcher->widget,
				  panel_launcher_motion_handler);
}

static void
launcher_section_done(void *data)
{
	struct panel *panel = data;
 
	if (key_launcher_icon == NULL || key_launcher_path == NULL) {
		fprintf(stderr, "invalid launcher section\n");
		return;
	}

	panel_add_launcher(panel,
				   key_launcher_icon, key_launcher_path);

	free(key_launcher_icon);
	key_launcher_icon = NULL;
	free(key_launcher_path);
	key_launcher_path = NULL;
}

struct panel *
panel_create(struct display *display, int id)
{
	struct panel *panel;

	panel = malloc(sizeof *panel);
	if (!panel)
		return NULL;
	memset(panel, 0, sizeof *panel);

	panel->base.configure = panel_configure;
	panel->window = window_create_custom(display);
	panel->widget = window_add_widget(panel->window, panel);
	wl_list_init(&panel->launcher_list);

	window_set_title(panel->window, "panel");
	window_set_user_data(panel->window, panel);

	widget_set_redraw_handler(panel->widget, panel_redraw_handler);
	widget_set_resize_handler(panel->widget, panel_resize_handler);
	widget_set_button_handler(panel->widget, panel_button_handler);
	
	panel_add_clock(panel);

	return panel;
}

void
panel_destroy(struct panel *panel)
{
	struct panel_launcher *tmp;
	struct panel_launcher *launcher;

	panel_destroy_clock(panel->clock);

	wl_list_for_each_safe(launcher, tmp, &panel->launcher_list, link)
		panel_destroy_launcher(launcher);

	widget_destroy(panel->widget);
	window_destroy(panel->window);

	free(panel);
}

int
panel_read_config(struct panel *panel, int config_fd)
{
	int ret;

	ret = parse_config_file(config_fd,
				config_sections, ARRAY_LENGTH(config_sections),
				&panel);
	if (ret < 0)
		panel_add_launcher(panel,
				   DATADIR "/weston/terminal.png",
				   BINDIR "/weston-terminal");
	return ret;
}

int
panel_painted(struct panel *panel)
{
	return panel->painted;
}

struct window *
panel_window(struct panel *panel)
{
	return panel->window;
}
