#include <glib.h>
#include <unistd.h>

#include <spice-server/spice.h>
#include <stdio.h>
#include <stdbool.h>
#include <ws2tcpip.h>

#include "display.h"

GMutex lock;
int draw_command_in_progress;

GAsyncQueue *cursor_queue;
GAsyncQueue *draw_queue;

struct SpiceTimer {
	SpiceTimerFunc func;
	void *opaque;
	GSource *source;
};

static SpiceTimer *timer_add(SpiceTimerFunc func, void *opaque)
{
	SpiceTimer *timer = (SpiceTimer *) calloc(1, sizeof(SpiceTimer));

	timer->func = func;
	timer->opaque = opaque;

	return timer;
}

static gboolean timer_func(gpointer user_data)
{
	SpiceTimer *timer = user_data;

	timer->func(timer->opaque);
	/* timer might be free after func(), don't touch */

	return FALSE;
}

static void timer_cancel(SpiceTimer *timer)
{
	if (timer->source) {
		g_source_destroy(timer->source);
		g_source_unref(timer->source);
		timer->source = NULL;
	}
}

static void timer_start(SpiceTimer *timer, uint32_t ms)
{
	timer_cancel(timer);

	timer->source = g_timeout_source_new(ms);

	g_source_set_callback(timer->source, timer_func, timer, NULL);

	g_source_attach(timer->source, g_main_context_default());

}

static void timer_remove(SpiceTimer *timer)
{
	timer_cancel(timer);
	free(timer);
}

struct SpiceWatch {
	void *opaque;
	GSource *source;
	GIOChannel *channel;
	SpiceWatchFunc func;
};

static GIOCondition spice_event_to_giocondition(int event_mask)
{
	GIOCondition condition = 0;

	if (event_mask & SPICE_WATCH_EVENT_READ)
		condition |= G_IO_IN;
	if (event_mask & SPICE_WATCH_EVENT_WRITE)
		condition |= G_IO_OUT;

	return condition;
}

static int giocondition_to_spice_event(GIOCondition condition)
{
	int event = 0;

	if (condition & G_IO_IN)
		event |= SPICE_WATCH_EVENT_READ;
	if (condition & G_IO_OUT)
		event |= SPICE_WATCH_EVENT_WRITE;

	return event;
}

static gboolean watch_func(GIOChannel *source, GIOCondition condition, gpointer data)
{
	SpiceWatch *watch = data;
	int fd = g_io_channel_unix_get_fd(source);

	watch->func(fd, giocondition_to_spice_event(condition), watch->opaque);

	return TRUE;
}

static void watch_update_mask(SpiceWatch *watch, int event_mask)
{
	if (watch->source) {
		g_source_destroy(watch->source);
		g_source_unref(watch->source);
		watch->source = NULL;
	}

	if (!event_mask)
		return;

	watch->source = g_io_create_watch(watch->channel, spice_event_to_giocondition(event_mask));
	g_source_set_callback(watch->source, (GSourceFunc) watch_func, watch, NULL);
	g_source_attach(watch->source, g_main_context_default());
}

static SpiceWatch *watch_add(int fd, int event_mask, SpiceWatchFunc func, void *opaque)
{
	SpiceWatch *watch;

	watch = calloc(1, sizeof(SpiceWatch));
	watch->channel = g_io_channel_unix_new(fd);
	watch->func = func;
	watch->opaque = opaque;

	watch_update_mask(watch, event_mask);

	return watch;
}

static void watch_remove(SpiceWatch *watch)
{
	watch_update_mask(watch, 0);

	g_io_channel_unref(watch->channel);
	free(watch);
}

static void channel_event(int event, SpiceChannelEventInfo *info)
{
	printf("channel event %d [connection_id %d|type %d|id %d|flags %d]\n",
			event, info->connection_id, info->type, info->id, info->flags);
	if (event == SPICE_CHANNEL_EVENT_INITIALIZED && info->type == SPICE_CHANNEL_MAIN) {
		char from[NI_MAXHOST + NI_MAXSERV + 128];
		strcpy(from, "Remote");
		if (info->flags & SPICE_CHANNEL_EVENT_FLAG_ADDR_EXT) {
			int rc;
			char host[NI_MAXHOST];
			char server[NI_MAXSERV];
			rc = getnameinfo((struct sockaddr *) &info->paddr_ext, info->plen_ext, host,
							 sizeof(host), server, sizeof(server), 0);
			if (rc == 0)
				snprintf(from, sizeof(from), "%s:%s", host, server);
		}
		printf("connect %s\n", from);
	}

	if (event == SPICE_CHANNEL_EVENT_DISCONNECTED && info->type == SPICE_CHANNEL_MAIN)
		printf("disconnect\n");
}

SpiceCoreInterface core = {
	.base = {
			 .major_version = SPICE_INTERFACE_CORE_MAJOR,
			 .minor_version = SPICE_INTERFACE_CORE_MINOR,
			 },
	.timer_add = timer_add,
	.timer_start = timer_start,
	.timer_cancel = timer_cancel,
	.timer_remove = timer_remove,
	.watch_add = watch_add,
	.watch_update_mask = watch_update_mask,
	.watch_remove = watch_remove,
	.channel_event = channel_event
};

static void kbd_push_key(SpiceKbdInstance *sin, uint8_t frag)
{
	static bool is_extendedkey = false;
	DWORD dwFlags = KEYEVENTF_SCANCODE
		| (is_extendedkey ? KEYEVENTF_EXTENDEDKEY : 0)
		| ((frag & 0x80) ? KEYEVENTF_KEYUP : 0);

	if (frag ==224) {
		is_extendedkey = true;
		return;
	}

	INPUT in = {
		.type = INPUT_KEYBOARD,
		.ki.dwFlags = dwFlags,
		.ki.wScan = frag & 0x7f,
		.ki.time = 0
	};

	SendInput(1, &in, sizeof(in));

	is_extendedkey = false;
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
	return SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK;
}

static const SpiceKbdInterface keyboard_sif = {
	.base.type = SPICE_INTERFACE_KEYBOARD,
	.base.description = "kuemmel keyboard",
	.base.major_version = SPICE_INTERFACE_KEYBOARD_MAJOR,
	.base.minor_version = SPICE_INTERFACE_KEYBOARD_MINOR,
	.push_scan_freg = kbd_push_key,
	.get_leds = kbd_get_leds,
};

static SpiceKbdInstance keyboard_sin = {
	.base.sif = &keyboard_sif.base,
};

void tablet_set_logical_size(SpiceTabletInstance *tablet G_GNUC_UNUSED, int width, int height)
{
	g_debug("TODO: %s UNIMPLEMENTED. (width %dx%d)", __func__, width, height);
}

void tablet_buttons(SpiceTabletInstance *tablet, uint32_t buttons_state)
{
	static uint32_t last_buttons_state = 0;
	DWORD dwFlags = 0;

	if ((last_buttons_state & SPICE_MOUSE_BUTTON_LEFT) && !(buttons_state  & SPICE_MOUSE_BUTTON_LEFT))
		dwFlags |= MOUSEEVENTF_LEFTUP;
	if (!(last_buttons_state & SPICE_MOUSE_BUTTON_LEFT) && (buttons_state  & SPICE_MOUSE_BUTTON_LEFT))
		dwFlags |= MOUSEEVENTF_LEFTDOWN;
	if ((last_buttons_state & SPICE_MOUSE_BUTTON_MIDDLE) && !(buttons_state  & SPICE_MOUSE_BUTTON_MIDDLE))
		dwFlags |= MOUSEEVENTF_MIDDLEUP;
	if (!(last_buttons_state & SPICE_MOUSE_BUTTON_MIDDLE) && (buttons_state  & SPICE_MOUSE_BUTTON_MIDDLE))
		dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
	if ((last_buttons_state & SPICE_MOUSE_BUTTON_RIGHT) && !(buttons_state  & SPICE_MOUSE_BUTTON_RIGHT))
		dwFlags |= MOUSEEVENTF_RIGHTUP;
	if (!(last_buttons_state & SPICE_MOUSE_BUTTON_RIGHT) && (buttons_state  & SPICE_MOUSE_BUTTON_RIGHT))
		dwFlags |= MOUSEEVENTF_RIGHTDOWN;

	last_buttons_state = buttons_state;

	INPUT in = {
		.type = INPUT_MOUSE,
		.mi.dwFlags = dwFlags,
		.mi.mouseData = 0,
		.mi.time = 0
	};

	SendInput(1, &in, sizeof(in));
}

void tablet_position(SpiceTabletInstance *tablet, int x, int y, uint32_t buttons_state)
{
	SetCursorPos(x, y);

	tablet_buttons(tablet, buttons_state);
}

void tablet_wheel(SpiceTabletInstance *tablet, int wheel_motion, uint32_t buttons_state)
{
	tablet_buttons(tablet, buttons_state);
}

static const SpiceTabletInterface tablet_sif = {
	.base.type = SPICE_INTERFACE_TABLET,
	.base.description = "kuemmel tablet",
	.base.major_version = SPICE_INTERFACE_TABLET_MAJOR,
	.base.minor_version = SPICE_INTERFACE_TABLET_MINOR,
	.set_logical_size = tablet_set_logical_size,
	.position = tablet_position,
	.wheel = tablet_wheel,
	.buttons = tablet_buttons,
};

static SpiceTabletInstance tablet_sin = {
	.base.sif = &tablet_sif.base,
};

static void attach_worker(QXLInstance *qin, QXLWorker *qxl_worker G_GNUC_UNUSED)
{
	static int count = 0;

	static QXLDevMemSlot slot = {
		.slot_group_id = 0,
		.slot_id = 0,
		.generation = 0,
		.virt_start = 0,
		.virt_end = ~0,
		.addr_delta = 0,
		.qxl_ram_size = ~0,
	};

	if (++count > 1) {
		g_message("Ignoring worker %d", count);
		return;
	}

	spice_qxl_add_memslot(qin, &slot);
}

static void set_compression_level(QXLInstance *qin, int level)
{
	// TODO - set_compression_level is unused
}

/* Newer spice servers no longer transmit this information,
	so let's just disregard it */
static void set_mm_time(QXLInstance *qin G_GNUC_UNUSED, uint32_t mm_time G_GNUC_UNUSED)
{
}

static void get_init_info(QXLInstance *qin G_GNUC_UNUSED, QXLDevInitInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->num_memslots = 1;
	info->num_memslots_groups = 1;
	info->memslot_id_bits = 1;
	info->memslot_gen_bits = 1;
	info->n_surfaces = 1;

	/* TODO - it would be useful to think through surface count a bit here */
}


static int get_command(QXLInstance *qin, struct QXLCommandExt *cmd)
{
	QXLDrawable *drawable;

	if (!g_mutex_trylock(&lock))
		return 0;

	drawable = g_async_queue_try_pop(draw_queue);
	draw_command_in_progress = (drawable != NULL);
	g_mutex_unlock(&lock);

	if (!drawable)
		return 0;

	cmd->group_id = 0;
	cmd->flags = 0;
	cmd->cmd.type = QXL_CMD_DRAW;
	cmd->cmd.padding = 0;
	cmd->cmd.data = (uintptr_t) drawable;

	return 1;
}

static int req_cmd_notification(QXLInstance *qin)
{
	int ret = 0;

	if (!g_mutex_trylock(&lock))
		return ret;

	ret = (g_async_queue_length(draw_queue)) > 0 ? 0 : 1;
	g_mutex_unlock(&lock);
	return (ret);
}

static void release_resource(QXLInstance *qin G_GNUC_UNUSED, struct QXLReleaseInfoExt release_info)
{
	release_asset(release_info.info);
}

static int get_cursor_command(QXLInstance *qin, struct QXLCommandExt *cmd)
{
	QXLCursorCmd *cursor_cmd;

	if (!g_mutex_trylock(&lock))
		return 0;

	cursor_cmd = g_async_queue_try_pop(cursor_queue);
	draw_command_in_progress = (cursor_cmd != NULL);
	g_mutex_unlock(&lock);

	if (!cursor_cmd)
		return 0;

	cmd->group_id = 0;
	cmd->flags = 0;
	cmd->cmd.type = QXL_CMD_CURSOR;
	cmd->cmd.padding = 0;
	cmd->cmd.data = (uintptr_t) cursor_cmd;

	return 1;
}

static int req_cursor_notification(QXLInstance *qin)
{
	return (g_async_queue_length(cursor_queue) > 0) ? 0 : 1;
}

static void notify_update(QXLInstance *qin G_GNUC_UNUSED, uint32_t update_id G_GNUC_UNUSED)
{
	g_debug("TODO: %s UNIMPLEMENTED", __func__);
}

static int flush_resources(QXLInstance *qin G_GNUC_UNUSED)
{
	g_debug("TODO: %s UNIMPLEMENTEDs", __func__);
	// Return 0 to direct the server to flush resources
	return 1;
}

static void async_complete(QXLInstance *qin G_GNUC_UNUSED, uint64_t cookie)
{
	g_debug("TODO: %s UNIMPLEMENTED!", __func__);
}

static void update_area_complete(QXLInstance *qin G_GNUC_UNUSED,
								 uint32_t surface_id G_GNUC_UNUSED,
								 struct QXLRect *updated_rects G_GNUC_UNUSED,
								 uint32_t num_updated_rects G_GNUC_UNUSED)
{
	g_debug("TODO: %s UNIMPLEMENTED!", __func__);
}

static const QXLInterface display_sif = {
	.base = {
			 .type = SPICE_INTERFACE_QXL,
			 .description = "kuemmel qxl",
			 .major_version = SPICE_INTERFACE_QXL_MAJOR,
			 .minor_version = SPICE_INTERFACE_QXL_MINOR},
	.attache_worker = attach_worker,
	.set_compression_level = set_compression_level,
	.set_mm_time = set_mm_time,
	.get_init_info = get_init_info,

	/* the callbacks below are called from spice server thread context */
	.get_command = get_command,
	.req_cmd_notification = req_cmd_notification,
	.release_resource = release_resource,
	.get_cursor_command = get_cursor_command,
	.req_cursor_notification = req_cursor_notification,
	.notify_update = notify_update,
	.flush_resources = flush_resources,
	.async_complete = async_complete,
	.update_area_complete = update_area_complete,
	.client_monitors_config = NULL, /* Specifying NULL here causes
										the better logic in the agent
										to operate */
	.set_client_capabilities = NULL,    /* Allowed to be unset */
};

static QXLInstance display_sin = {
	.base.sif = &display_sif.base,
};

static int send_monitors_config(int w, int h)
{
	QXLMonitorsConfig *monitors = calloc(1, sizeof(QXLMonitorsConfig) + sizeof(QXLHead));
	if (!monitors)
		return -ENOMEM;

	monitors->count = 1;
	monitors->max_allowed = 1;
	monitors->heads[0].id = 0;
	monitors->heads[0].surface_id = 0;
	monitors->heads[0].width = w;
	monitors->heads[0].height = h;

	spice_qxl_monitors_config_async(&display_sin, (uintptr_t) monitors, 0, 0);

	return 0;
}

int spice_create_primary(int w, int h, int bytes_per_line, void *shmaddr)
{
	QXLDevSurfaceCreate surface = { };

	surface.height = h;
	surface.width = w;

	surface.stride = -1 * bytes_per_line;
	surface.type = QXL_SURF_TYPE_PRIMARY;
	surface.flags = 0;
	surface.group_id = 0;
	surface.mouse_mode = TRUE;

	// Position appears to be completely unused
	surface.position = 0;

	/* TODO - compute this dynamically */
	surface.format = SPICE_SURFACE_FMT_32_xRGB;
	surface.mem = (uintptr_t) malloc(h*bytes_per_line);

	spice_qxl_create_primary_surface(&display_sin, 0, &surface);

	return send_monitors_config(w, h);
}

void spice_destroy_primary()
{
	spice_qxl_destroy_primary_surface(&display_sin, 0);
}

int main(int argc, char** argv)
{
	draw_queue = g_async_queue_new();
	cursor_queue = g_async_queue_new();
	g_mutex_init(&lock);

	struct display_config display_config = { &display_sin, draw_queue, cursor_queue };

	printf("v %d\n", spice_get_current_compat_version());
	SpiceServer *server = spice_server_new();
	if (!server)
		exit(EXIT_FAILURE);
	printf("server %p\n", server);

	spice_server_set_port(server, 19191);
	spice_server_set_noauth(server);
	spice_server_set_name(server, "kuemmel");

	if (spice_server_init(server, &core) < 0) {
		spice_server_destroy(server);
		exit(EXIT_FAILURE);
	}

	if (spice_server_add_interface(server, &keyboard_sin.base)) {
		spice_server_destroy(server);
		exit(EXIT_FAILURE);
	}

	if (spice_server_add_interface(server, &tablet_sin.base)) {
		spice_server_destroy(server);
		exit(EXIT_FAILURE);
	}

	if (spice_server_add_interface(server, &display_sin.base)) {
		spice_server_destroy(server);
		exit(EXIT_FAILURE);
	}

	spice_server_vm_start(server);

	spice_create_primary(1920, 1080, 1920*4, NULL);

	g_thread_new("display", display, &display_config);

	GMainLoop *loop = g_main_loop_new (NULL, FALSE);

	g_main_loop_run (loop);
}
