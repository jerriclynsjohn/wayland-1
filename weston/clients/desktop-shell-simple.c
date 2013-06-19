/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Collabora, Ltd.
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
#include <sys/wait.h>
#include <sys/epoll.h> 
#include <linux/input.h>
#include <libgen.h>
#include <ctype.h>
#include <time.h>

#include <wayland-client.h>
#include "window.h"
#include "../shared/cairo-util.h"
#include "../shared/config-parser.h"

#include "desktop-shell-client-protocol.h"
#include "desktop-shell.h"

struct desktop {
	struct display *display;
	struct desktop_shell *shell;
	uint32_t interface_version;
	struct unlocker *unlocker;
	struct wl_list outputs;

	struct window *grab_window;
	struct widget *grab_widget;

	enum cursor_type grab_cursor;

	int painted;
};

struct output {
	struct wl_output *output;
	struct wl_list link;

	struct panel *panel;
	struct background *background;
};

static int key_locking = 1;

static void
sigchild_handler(int s)
{
	int status;
	pid_t pid;

	while (pid = waitpid(-1, &status, WNOHANG), pid > 0)
		fprintf(stderr, "child %d exited\n", pid);
}

struct desktop_shell *
desktop_shell(struct desktop *desktop)
{
	return desktop->shell;
}

struct display *
desktop_display(struct desktop *desktop)
{
	return desktop->display;
}

static int
is_desktop_painted(struct desktop *desktop)
{
	struct output *output;

	wl_list_for_each(output, &desktop->outputs, link) {
		if (output->panel && !panel_painted(output->panel))
			return 0;
		if (output->background && !background_painted(output->background))
			return 0;
	}

	return 1;
}

void
check_desktop_ready(struct window *window)
{
	struct display *display;
	struct desktop *desktop;

	display = window_get_display(window);
	desktop = display_get_user_data(display);

	if (!desktop->painted && is_desktop_painted(desktop)) {
		desktop->painted = 1;

		if (desktop->interface_version >= 2)
			desktop_shell_desktop_ready(desktop->shell);
	}
}


static void
desktop_shell_configure(void *data,
			struct desktop_shell *desktop_shell,
			uint32_t edges,
			struct wl_surface *surface,
			int32_t width, int32_t height)
{
	struct window *window = wl_surface_get_user_data(surface);
	struct surface *s = window_get_user_data(window);

	s->configure(data, desktop_shell, edges, window, width, height);
}

static void
desktop_shell_prepare_lock_surface(void *data,
				   struct desktop_shell *desktop_shell)
{
	struct desktop *desktop = data;

	if (!key_locking || !desktop->unlocker) {
		desktop_shell_unlock(desktop->shell);
		return;
	}

	unlocker_lock(desktop->unlocker);
}

static void
desktop_shell_grab_cursor(void *data,
			  struct desktop_shell *desktop_shell,
			  uint32_t cursor)
{
	struct desktop *desktop = data;

	switch (cursor) {
	case DESKTOP_SHELL_CURSOR_NONE:
		desktop->grab_cursor = CURSOR_BLANK;
		break;
	case DESKTOP_SHELL_CURSOR_BUSY:
		desktop->grab_cursor = CURSOR_WATCH;
		break;
	case DESKTOP_SHELL_CURSOR_MOVE:
		desktop->grab_cursor = CURSOR_DRAGGING;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP:
		desktop->grab_cursor = CURSOR_TOP;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM:
		desktop->grab_cursor = CURSOR_BOTTOM;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_LEFT:
		desktop->grab_cursor = CURSOR_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_RIGHT:
		desktop->grab_cursor = CURSOR_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_LEFT:
		desktop->grab_cursor = CURSOR_TOP_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_TOP_RIGHT:
		desktop->grab_cursor = CURSOR_TOP_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_LEFT:
		desktop->grab_cursor = CURSOR_BOTTOM_LEFT;
		break;
	case DESKTOP_SHELL_CURSOR_RESIZE_BOTTOM_RIGHT:
		desktop->grab_cursor = CURSOR_BOTTOM_RIGHT;
		break;
	case DESKTOP_SHELL_CURSOR_ARROW:
	default:
		desktop->grab_cursor = CURSOR_LEFT_PTR;
	}
}

static const struct desktop_shell_listener listener = {
	desktop_shell_configure,
	desktop_shell_prepare_lock_surface,
	desktop_shell_grab_cursor
};

static int
grab_surface_enter_handler(struct widget *widget, struct input *input,
			   float x, float y, void *data)
{
	struct desktop *desktop = data;

	return desktop->grab_cursor;
}

static void
grab_surface_destroy(struct desktop *desktop)
{
	widget_destroy(desktop->grab_widget);
	window_destroy(desktop->grab_window);
}

static void
grab_surface_create(struct desktop *desktop)
{
	struct wl_surface *s;

	desktop->grab_window = window_create_custom(desktop->display);
	window_set_user_data(desktop->grab_window, desktop);

	s = window_get_wl_surface(desktop->grab_window);
	desktop_shell_set_grab_surface(desktop->shell, s);

	desktop->grab_widget =
		window_add_widget(desktop->grab_window, desktop);
	/* We set the allocation to 1x1 at 0,0 so the fake enter event
	 * at 0,0 will go to this widget. */
	widget_set_allocation(desktop->grab_widget, 0, 0, 1, 1);

	widget_set_enter_handler(desktop->grab_widget,
				 grab_surface_enter_handler);
}

static void
output_destroy(struct output *output)
{
	if (output->background) background_destroy(output->background);
	if (output->panel) panel_destroy(output->panel);
	wl_output_destroy(output->output);
	wl_list_remove(&output->link);

	free(output);
}

static void
desktop_destroy_outputs(struct desktop *desktop)
{
	struct output *tmp;
	struct output *output;

	wl_list_for_each_safe(output, tmp, &desktop->outputs, link)
		output_destroy(output);
}

static void
output_handle_geometry(void *data,
                       struct wl_output *wl_output,
                       int x, int y,
                       int physical_width,
                       int physical_height,
                       int subpixel,
                       const char *make,
                       const char *model,
                       int transform)
{
	struct output *output = data;

	if (output->panel)
		window_set_buffer_transform(panel_window(output->panel),
							transform);

	if (output->background)
		window_set_buffer_transform(background_window(output->background),
							transform);
}

static void
output_handle_mode(void *data,
		   struct wl_output *wl_output,
		   uint32_t flags,
		   int width,
		   int height,
		   int refresh)
{
}

static void
output_handle_done(void *data,
                   struct wl_output *wl_output)
{
}

static void
output_handle_scale(void *data,
                    struct wl_output *wl_output,
                    int32_t scale)
{
	struct output *output = data;

	if (output->panel)
		window_set_buffer_scale(panel_window(output->panel),
							scale);
	if (output->background)
		window_set_buffer_scale(background_window(output->background),
							scale);
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale
};

static struct output *
output_create(struct desktop *desktop, uint32_t id)
{
	struct output *output;

	output = calloc(1, sizeof *output);
	if (!output)
		return NULL;

	output->output =
		display_bind(desktop->display, id, &wl_output_interface, 2);

	wl_output_add_listener(output->output, &output_listener, output);

	return output;
}

static void
global_handler(struct display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct desktop *desktop = data;

	if (!strcmp(interface, "desktop_shell")) {
		desktop->interface_version = (version < 2) ? version : 2;
		desktop->shell = display_bind(desktop->display,
					      id, &desktop_shell_interface,
					      desktop->interface_version);
		desktop_shell_add_listener(desktop->shell, &listener, desktop);
	} else if (!strcmp(interface, "wl_output")) {
		struct output *output;
		struct wl_surface *surface;
		output = output_create(desktop, id);
		wl_list_insert(&desktop->outputs, &output->link);

		output->panel = panel_create(desktop->display, id);
		if (output->panel) {
			surface = window_get_wl_surface(panel_window(output->panel));
			desktop_shell_set_panel(desktop->shell,
						output->output, surface);
		}
		output->background = background_create(desktop, id);
		if (output->background) {
			surface = window_get_wl_surface(background_window(output->background));
			desktop_shell_set_background(desktop->shell,
					     output->output, surface);
		}
	}
}

int main(int argc, char *argv[])
{
	struct desktop desktop = { 0 };
	int config_fd;
	struct output *output;

	desktop.unlocker = unlocker_create(&desktop);
	wl_list_init(&desktop.outputs);

	desktop.display = display_create(&argc, argv);
	if (desktop.display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return -1;
	}

	display_set_user_data(desktop.display, &desktop);
	display_set_global_handler(desktop.display, global_handler);

	grab_surface_create(&desktop);

	config_fd = open_config_file("weston.ini");
	wl_list_for_each(output, &desktop.outputs, link) {
		if (output->panel)
			panel_read_config(output->panel, config_fd);
		if (output->background)
			background_read_config(output->background, config_fd);
	}
	close(config_fd);

	signal(SIGCHLD, sigchild_handler);

	display_run(desktop.display);

	/* Cleanup */
	grab_surface_destroy(&desktop);
	desktop_destroy_outputs(&desktop);
	if (desktop.unlocker)
		unlocker_destroy(desktop.unlocker);
	desktop_shell_destroy(desktop.shell);
	display_destroy(desktop.display);

	return 0;
}
