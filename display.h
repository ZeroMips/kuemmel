#pragma once

struct display_config {
	QXLInstance *display_sin;
	GAsyncQueue *draw_queue;
	GAsyncQueue *cursor_queue;
};

#ifdef __cplusplus
extern "C"
{
#endif

void release_asset(void *asset);
gpointer display(gpointer data);

#ifdef __cplusplus
} // extern "C"
#endif