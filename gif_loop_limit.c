/*
 * GIF Loop Limit Plugin for Pidgin
 *
 * Prevents GIF images in conversation windows from looping indefinitely.
 * Pauses after 3 loops and displays a play icon overlay.
 * Clicking a paused GIF resumes animation for 2 loops.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef GDK_PIXBUF_ENABLE_BACKEND
# define GDK_PIXBUF_ENABLE_BACKEND 1
#endif

#include <glib.h>
#include <glib-object.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-animation.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include <plugin.h>
#include <version.h>
#include <conversation.h>
#include <debug.h>
#include <gtkplugin.h>
#include <gtkconv.h>
#include <gtkimhtml.h>
#include <gtkprefs.h>
#include <gtkutils.h>

#ifndef _
# define _(String) (String)
#endif
#ifndef N_
# define N_(String) (String)
#endif

#define PLUGIN_ID           "gtk-gif-loop-limit"
#define PLUGIN_NAME         "GIF Loop Limit"
#define PLUGIN_STATIC_NAME  gif_loop_limit
#define PLUGIN_SUMMARY      "Limits animated GIFs from looping indefinitely"
#define PLUGIN_DESCRIPTION  "Stops animated GIFs after 3 loops and displays a play button overlay. Clicking the GIF resumes playback for 2 loops."
#define PLUGIN_AUTHOR       "Eion Robb"
#define PLUGIN_VERSION      "0.1"

#define PREFS_PREFIX        "/plugins/gtk/" PLUGIN_ID
#define PREF_INITIAL_LOOPS  PREFS_PREFIX "/initial_loops"
#define PREF_RESUME_LOOPS   PREFS_PREFIX "/resume_loops"
#define PREF_SHOW_PLAY_ICON PREFS_PREFIX "/show_play_icon"

/* Private Pidgin structure from gtkimhtml.c */
struct scalable_data {
	GtkIMHtmlScalable *scalable;
	GtkTextMark *mark;
};

/* Internal gdk-pixbuf structures for GIF animation iterators */
typedef struct _GdkPixbufGifAnim GdkPixbufGifAnim;
typedef struct _GdkPixbufGifAnimIter GdkPixbufGifAnimIter;
typedef struct _GdkPixbufFrame GdkPixbufFrame;

struct _GdkPixbufGifAnim {
	GdkPixbufAnimation parent_instance;
	int total_time;
	guchar color_map[256 * 3];
	GList *frames;
	int width, height;
	int loop;
	GdkPixbuf *last_frame_data;
	GdkPixbufFrame *last_frame;
	GdkPixbuf *last_frame_revert_data;
};

struct _GdkPixbufGifAnimIter {
	GdkPixbufAnimationIter parent_instance;
	GdkPixbufGifAnim   *gif_anim;
	GTimeVal            start_time;
	GTimeVal            current_time;
	gint                position;
	GList              *current_frame;
	gint                first_loop_slowness;
};

typedef struct _GifLoopData {
	GtkIMHtmlAnimation *anim;
	GtkWidget          *event_box;
	GtkWidget          *image_widget;
	GdkPixbufAnimationIter *iter;

	int                 loop_limit;
	int                 current_loop;
	gboolean            is_paused;

	guint               idle_pause_id;
	guint               custom_timer_id;

	GList              *last_frame;
	gint                last_position;

	GdkPixbuf          *saved_clean_pixbuf;
	gulong              event_handler_id;
} GifLoopData;

static gboolean (*orig_advance)(GdkPixbufAnimationIter *iter, const GTimeVal *current_time) = NULL;
static GdkPixbufAnimationIterClass *hooked_iter_class = NULL;
#ifdef _WIN32
static void *real_animation_free = NULL;
#endif

static GList *active_loop_data_list = NULL;

static void ensure_hooked(GdkPixbufAnimationIter *iter);
static void gif_overlay_play_icon(GifLoopData *data);
static void gif_remove_play_overlay(GifLoopData *data);
static void gif_resume_playback(GifLoopData *data);
static gboolean pause_animation_idle_cb(gpointer user_data);
static gboolean event_box_event_cb(GtkWidget *widget, GdkEvent *event, gpointer user_data);
static void gif_hook_imhtml(GtkIMHtml *imhtml);

static gboolean
is_imhtml_animation(GtkIMHtmlScalable *scale)
{
	if (!scale)
		return FALSE;
#ifdef _WIN32
	if (real_animation_free && (void *)scale->free == real_animation_free)
		return TRUE;
#endif
	if (scale->free == gtk_imhtml_animation_free)
		return TRUE;
	return FALSE;
}

static GifLoopData *
gif_loop_data_new(GtkIMHtmlAnimation *anim)
{
	GifLoopData *data = g_new0(GifLoopData, 1);
	data->anim = anim;
	data->loop_limit = purple_prefs_get_int(PREF_INITIAL_LOOPS);
	if (data->loop_limit <= 0)
		data->loop_limit = 3;
	data->current_loop = 0;
	data->is_paused = FALSE;
	data->last_position = -1;

	if (anim) {
		data->iter = anim->iter;
		if (anim->imhtmlimage.image) {
			data->image_widget = GTK_WIDGET(anim->imhtmlimage.image);
			data->event_box = gtk_widget_get_parent(data->image_widget);

			if (data->event_box && GTK_IS_EVENT_BOX(data->event_box)) {
				data->event_handler_id = g_signal_connect(G_OBJECT(data->event_box),
					"event", G_CALLBACK(event_box_event_cb), data);
			}
		}
	}

	active_loop_data_list = g_list_prepend(active_loop_data_list, data);
	return data;
}

static void
gif_loop_data_free(GifLoopData *data)
{
	if (!data)
		return;

	active_loop_data_list = g_list_remove(active_loop_data_list, data);

	if (data->idle_pause_id > 0) {
		g_source_remove(data->idle_pause_id);
		data->idle_pause_id = 0;
	}
	if (data->custom_timer_id > 0) {
		g_source_remove(data->custom_timer_id);
		data->custom_timer_id = 0;
	}

	if (data->event_box && data->event_handler_id > 0) {
		if (g_signal_handler_is_connected(G_OBJECT(data->event_box), data->event_handler_id)) {
			g_signal_handler_disconnect(G_OBJECT(data->event_box), data->event_handler_id);
		}
		data->event_handler_id = 0;
	}

	if (data->event_box && GTK_WIDGET_REALIZED(data->event_box) && data->event_box->window) {
		gdk_window_set_cursor(data->event_box->window, NULL);
	}

	gif_remove_play_overlay(data);

	if (data->iter) {
		g_object_set_data(G_OBJECT(data->iter), "gif_loop_data", NULL);
	}

	g_free(data);
}

static void
gif_loop_data_attach_anim(GifLoopData *data, GtkIMHtmlAnimation *anim)
{
	if (!data || !anim)
		return;

	data->anim = anim;
	data->iter = anim->iter;

	if (anim->imhtmlimage.image) {
		data->image_widget = GTK_WIDGET(anim->imhtmlimage.image);
		data->event_box = gtk_widget_get_parent(data->image_widget);

		if (data->event_box && GTK_IS_EVENT_BOX(data->event_box) && data->event_handler_id == 0) {
			data->event_handler_id = g_signal_connect(G_OBJECT(data->event_box),
				"event", G_CALLBACK(event_box_event_cb), data);
		}
	}
}

static void
gif_find_and_attach_anim(GifLoopData *data, GdkPixbufAnimationIter *iter)
{
	GList *convs = purple_get_conversations();
	GList *cl;

	for (cl = convs; cl != NULL; cl = cl->next) {
		PurpleConversation *conv = cl->data;
		PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
		if (!gtkconv)
			continue;

		if (gtkconv->imhtml) {
			GtkIMHtml *imhtml = GTK_IMHTML(gtkconv->imhtml);
			GList *sl;
			for (sl = imhtml->scalables; sl != NULL; sl = sl->next) {
				struct scalable_data *sd = sl->data;
				if (sd && sd->scalable && (is_imhtml_animation(sd->scalable) || ((GtkIMHtmlAnimation *)sd->scalable)->iter == iter)) {
					GtkIMHtmlAnimation *anim = (GtkIMHtmlAnimation *)sd->scalable;
					if (anim->iter == iter) {
						gif_loop_data_attach_anim(data, anim);
						return;
					}
				}
			}
		}

		if (gtkconv->entry) {
			GtkIMHtml *imhtml = GTK_IMHTML(gtkconv->entry);
			GList *sl;
			for (sl = imhtml->scalables; sl != NULL; sl = sl->next) {
				struct scalable_data *sd = sl->data;
				if (sd && sd->scalable && (is_imhtml_animation(sd->scalable) || ((GtkIMHtmlAnimation *)sd->scalable)->iter == iter)) {
					GtkIMHtmlAnimation *anim = (GtkIMHtmlAnimation *)sd->scalable;
					if (anim->iter == iter) {
						gif_loop_data_attach_anim(data, anim);
						return;
					}
				}
			}
		}
	}
}

static gboolean
gif_loop_limit_advance(GdkPixbufAnimationIter *iter, const GTimeVal *current_time)
{
	gboolean changed = FALSE;
	GifLoopData *data;

	if (orig_advance) {
		changed = orig_advance(iter, current_time);
	}

	data = g_object_get_data(G_OBJECT(iter), "gif_loop_data");
	if (!data) {
		data = gif_loop_data_new(NULL);
		data->iter = iter;
		g_object_set_data_full(G_OBJECT(iter), "gif_loop_data", data, (GDestroyNotify)gif_loop_data_free);
		gif_find_and_attach_anim(data, iter);
	} else if (!data->anim) {
		gif_find_and_attach_anim(data, iter);
	}

	if (data->is_paused) {
		return FALSE;
	}

	GdkPixbufGifAnimIter *gif_iter = (GdkPixbufGifAnimIter *)iter;
	if (gif_iter->gif_anim && gif_iter->gif_anim->total_time > 0) {
		if (data->last_frame != NULL &&
		    gif_iter->current_frame == gif_iter->gif_anim->frames &&
		    data->last_frame != gif_iter->current_frame) {
			data->current_loop++;
			purple_debug_info("gif_loop_limit", "Loop completed: %d/%d on iter %p\n",
				data->current_loop, data->loop_limit, iter);
		} else if (data->last_position > 0 &&
		           gif_iter->position < data->last_position &&
		           (data->last_position - gif_iter->position > gif_iter->gif_anim->total_time / 2)) {
			data->current_loop++;
			purple_debug_info("gif_loop_limit", "Loop completed (by position): %d/%d on iter %p\n",
				data->current_loop, data->loop_limit, iter);
		}

		data->last_frame = gif_iter->current_frame;
		data->last_position = gif_iter->position;

		if (data->current_loop >= data->loop_limit) {
			purple_debug_info("gif_loop_limit", "Pausing GIF after %d loops on iter %p\n",
				data->current_loop, iter);
			data->is_paused = TRUE;
			if (data->idle_pause_id == 0) {
				data->idle_pause_id = g_idle_add(pause_animation_idle_cb, data);
			}
			return FALSE;
		}
	}

	return changed;
}

static gboolean
pause_animation_idle_cb(gpointer user_data)
{
	GifLoopData *data = user_data;
	data->idle_pause_id = 0;

	if (!data->anim && data->iter) {
		gif_find_and_attach_anim(data, data->iter);
	}

	if (data->anim && data->anim->timer > 0) {
		g_source_remove(data->anim->timer);
		data->anim->timer = 0;
	}
	if (data->custom_timer_id > 0) {
		g_source_remove(data->custom_timer_id);
		data->custom_timer_id = 0;
	}

	data->is_paused = TRUE;

	if (data->anim && !data->event_box && data->anim->imhtmlimage.image) {
		data->image_widget = GTK_WIDGET(data->anim->imhtmlimage.image);
		data->event_box = gtk_widget_get_parent(data->image_widget);
		if (data->event_box && GTK_IS_EVENT_BOX(data->event_box) && data->event_handler_id == 0) {
			data->event_handler_id = g_signal_connect(G_OBJECT(data->event_box),
				"event", G_CALLBACK(event_box_event_cb), data);
		}
	}

	gif_overlay_play_icon(data);

	if (data->event_box && GTK_WIDGET_REALIZED(data->event_box) && data->event_box->window) {
		GdkCursor *cursor = gdk_cursor_new(GDK_HAND2);
		gdk_window_set_cursor(data->event_box->window, cursor);
		gdk_cursor_unref(cursor);
	}
	if (data->event_box) {
		gtk_widget_set_tooltip_text(data->event_box, _("Click to play GIF"));
	}

	return FALSE;
}

static void
gif_overlay_play_icon(GifLoopData *data)
{
	if (!purple_prefs_get_bool(PREF_SHOW_PLAY_ICON))
		return;

	if (!data->anim || !data->anim->imhtmlimage.image)
		return;

	GtkImage *gtk_img = data->anim->imhtmlimage.image;
	GdkPixbuf *current_pixbuf = gtk_image_get_pixbuf(gtk_img);
	if (!current_pixbuf)
		return;

	int w = gdk_pixbuf_get_width(current_pixbuf);
	int h = gdk_pixbuf_get_height(current_pixbuf);
	if (w < 8 || h < 8)
		return;

	if (data->saved_clean_pixbuf)
		g_object_unref(data->saved_clean_pixbuf);
	data->saved_clean_pixbuf = gdk_pixbuf_copy(current_pixbuf);

	GdkPixbuf *overlay;
	if (gdk_pixbuf_get_has_alpha(current_pixbuf)) {
		overlay = gdk_pixbuf_copy(current_pixbuf);
	} else {
		overlay = gdk_pixbuf_add_alpha(current_pixbuf, FALSE, 0, 0, 0);
	}

	int min_dim = MIN(w, h);
	int badge_radius = min_dim / 4;
	if (badge_radius > 24) badge_radius = 24;
	if (badge_radius < 10) badge_radius = 10;
	if (badge_radius * 2 > min_dim) badge_radius = min_dim / 2;

	int cx = w / 2;
	int cy = h / 2;

	int x1 = MAX(0, cx - badge_radius);
	int x2 = MIN(w - 1, cx + badge_radius);
	int y1 = MAX(0, cy - badge_radius);
	int y2 = MIN(h - 1, cy + badge_radius);

	guchar *pixels = gdk_pixbuf_get_pixels(overlay);
	int rowstride = gdk_pixbuf_get_rowstride(overlay);
	int n_channels = gdk_pixbuf_get_n_channels(overlay);
	int r2 = badge_radius * badge_radius;

	for (int y = y1; y <= y2; y++) {
		guchar *row = pixels + y * rowstride;
		int dy = y - cy;
		for (int x = x1; x <= x2; x++) {
			int dx = x - cx;
			int dist2 = dx * dx + dy * dy;
			if (dist2 <= r2) {
				guchar *p = row + x * n_channels;
				p[0] = (p[0] * 60) / 255;
				p[1] = (p[1] * 60) / 255;
				p[2] = (p[2] * 60) / 255;
				if (n_channels == 4 && p[3] < 210) {
					p[3] = 210;
				}
			}
		}
	}

	int tri_size = badge_radius * 2 / 3;
	if (tri_size < 5) tri_size = 5;

	int left_x = cx - tri_size / 2;
	int right_x = cx + tri_size;
	int tri_w = right_x - left_x;

	for (int y = cy - tri_size; y <= cy + tri_size; y++) {
		if (y < 0 || y >= h) continue;
		int dy = ABS(y - cy);
		int cur_w = (tri_size > 0) ? (tri_w * (tri_size - dy)) / tri_size : 0;
		int cur_right = left_x + cur_w;

		guchar *row = pixels + y * rowstride;
		for (int x = left_x; x <= cur_right; x++) {
			if (x >= 0 && x < w) {
				guchar *p = row + x * n_channels;
				p[0] = 255;
				p[1] = 255;
				p[2] = 255;
				if (n_channels == 4) p[3] = 255;
			}
		}
	}

	gtk_image_set_from_pixbuf(gtk_img, overlay);
	g_object_unref(overlay);
}

static void
gif_remove_play_overlay(GifLoopData *data)
{
	if (data->saved_clean_pixbuf && data->anim && data->anim->imhtmlimage.image) {
		gtk_image_set_from_pixbuf(data->anim->imhtmlimage.image, data->saved_clean_pixbuf);
		g_object_unref(data->saved_clean_pixbuf);
		data->saved_clean_pixbuf = NULL;
	}
}

static gboolean
gif_playback_timer_cb(gpointer user_data)
{
	GifLoopData *data = user_data;
	if (!data || !data->anim)
		return FALSE;

	GtkIMHtmlAnimation *anim = data->anim;

	if (data->is_paused) {
		data->custom_timer_id = 0;
		return FALSE;
	}

	if (gdk_pixbuf_animation_iter_advance(anim->iter, NULL)) {
		GdkPixbuf *pb = gdk_pixbuf_animation_iter_get_pixbuf(anim->iter);
		if (anim->imhtmlimage.pixbuf)
			g_object_unref(anim->imhtmlimage.pixbuf);
		anim->imhtmlimage.pixbuf = gdk_pixbuf_copy(pb);

		int width = gdk_pixbuf_get_width(gtk_image_get_pixbuf(anim->imhtmlimage.image));
		int height = gdk_pixbuf_get_height(gtk_image_get_pixbuf(anim->imhtmlimage.image));
		if (width > 0 && height > 0) {
			GdkPixbuf *tmp = gdk_pixbuf_scale_simple(anim->imhtmlimage.pixbuf, width, height, GDK_INTERP_BILINEAR);
			gtk_image_set_from_pixbuf(anim->imhtmlimage.image, tmp);
			g_object_unref(tmp);
		} else {
			gtk_image_set_from_pixbuf(anim->imhtmlimage.image, anim->imhtmlimage.pixbuf);
		}
	}

	if (data->is_paused) {
		data->custom_timer_id = 0;
		return FALSE;
	}

	int delay = gdk_pixbuf_animation_iter_get_delay_time(anim->iter);
	if (delay <= 0) delay = 100;
	delay = MIN(delay, 100);

	data->custom_timer_id = g_timeout_add(delay, gif_playback_timer_cb, data);
	return FALSE;
}

static void
gif_resume_playback(GifLoopData *data)
{
	if (!data || !data->anim)
		return;

	purple_debug_info("gif_loop_limit", "Resuming GIF playback on iter %p\n", data->iter);

	if (data->idle_pause_id > 0) {
		g_source_remove(data->idle_pause_id);
		data->idle_pause_id = 0;
	}
	if (data->anim->timer > 0) {
		g_source_remove(data->anim->timer);
		data->anim->timer = 0;
	}
	if (data->custom_timer_id > 0) {
		g_source_remove(data->custom_timer_id);
		data->custom_timer_id = 0;
	}

	gif_remove_play_overlay(data);

	if (data->event_box && GTK_WIDGET_REALIZED(data->event_box) && data->event_box->window) {
		gdk_window_set_cursor(data->event_box->window, NULL);
	}
	if (data->event_box) {
		gtk_widget_set_tooltip_text(data->event_box, NULL);
	}

	data->loop_limit = purple_prefs_get_int(PREF_RESUME_LOOPS);
	if (data->loop_limit <= 0)
		data->loop_limit = 2;
	data->current_loop = 0;
	data->is_paused = FALSE;
	data->last_frame = NULL;
	data->last_position = -1;

	GdkPixbufGifAnimIter *gif_iter = (GdkPixbufGifAnimIter *)data->anim->iter;
	if (gif_iter && gif_iter->gif_anim) {
		g_get_current_time(&gif_iter->start_time);
		gif_iter->first_loop_slowness = 0;
		gif_iter->position = 0;
		gif_iter->current_frame = gif_iter->gif_anim->frames;
	}

	int delay = gdk_pixbuf_animation_iter_get_delay_time(data->anim->iter);
	if (delay <= 0) delay = 100;
	delay = MIN(delay, 100);
	data->custom_timer_id = g_timeout_add(delay, gif_playback_timer_cb, data);
}

static gboolean
event_box_event_cb(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
	GifLoopData *data = user_data;
	if (!data)
		return FALSE;

	if (event->type == GDK_BUTTON_PRESS) {
		GdkEventButton *bev = (GdkEventButton *)event;
		if (bev->button == 1) {
			purple_debug_info("gif_loop_limit", "Left click on GIF (is_paused=%d)\n", data->is_paused);
			if (data->is_paused) {
				gif_resume_playback(data);
				return TRUE;
			} else {
				data->is_paused = TRUE;
				pause_animation_idle_cb(data);
				return TRUE;
			}
		}
	} else if (event->type == GDK_ENTER_NOTIFY) {
		if (data->is_paused && widget->window) {
			GdkCursor *cursor = gdk_cursor_new(GDK_HAND2);
			gdk_window_set_cursor(widget->window, cursor);
			gdk_cursor_unref(cursor);
		}
	} else if (event->type == GDK_LEAVE_NOTIFY) {
		if (widget->window) {
			gdk_window_set_cursor(widget->window, NULL);
		}
	}

	return FALSE;
}

static void
ensure_hooked(GdkPixbufAnimationIter *iter)
{
	if (!hooked_iter_class && iter) {
		GdkPixbufAnimationIterClass *klass = GDK_PIXBUF_ANIMATION_ITER_GET_CLASS(iter);
		if (klass && klass->advance && klass->advance != gif_loop_limit_advance) {
			orig_advance = klass->advance;
			hooked_iter_class = klass;
			g_type_class_ref(G_OBJECT_TYPE(iter));
			klass->advance = gif_loop_limit_advance;
			purple_debug_info("gif_loop_limit", "Hooked klass->advance: class=%p, orig=%p\n",
				klass, orig_advance);
		}
	}
}

static void
gif_hook_imhtml(GtkIMHtml *imhtml)
{
	if (!imhtml || !imhtml->scalables)
		return;

	GList *l;
	for (l = imhtml->scalables; l != NULL; l = l->next) {
		struct scalable_data *sd = l->data;
		if (!sd || !sd->scalable)
			continue;

		if (is_imhtml_animation(sd->scalable)) {
			GtkIMHtmlAnimation *anim = (GtkIMHtmlAnimation *)sd->scalable;
			if (anim->iter) {
				ensure_hooked(anim->iter);
				GifLoopData *data = g_object_get_data(G_OBJECT(anim->iter), "gif_loop_data");
				if (!data) {
					data = gif_loop_data_new(anim);
					g_object_set_data_full(G_OBJECT(anim->iter), "gif_loop_data", data, (GDestroyNotify)gif_loop_data_free);
				} else if (!data->anim) {
					gif_loop_data_attach_anim(data, anim);
				}
			}
		}
	}
}

static void
gif_hook_conversation(PurpleConversation *conv)
{
	if (!conv)
		return;

	PidginConversation *gtkconv = PIDGIN_CONVERSATION(conv);
	if (!gtkconv)
		return;

	if (gtkconv->imhtml) {
		gif_hook_imhtml(GTK_IMHTML(gtkconv->imhtml));
	}
	if (gtkconv->entry) {
		gif_hook_imhtml(GTK_IMHTML(gtkconv->entry));
	}
}

static void
conversation_displayed_cb(PidginConversation *gtkconv, gpointer user_data)
{
	if (gtkconv && gtkconv->imhtml) {
		gif_hook_imhtml(GTK_IMHTML(gtkconv->imhtml));
	}
}

static void
conversation_created_cb(PurpleConversation *conv, gpointer user_data)
{
	gif_hook_conversation(conv);
}

static void
displayed_msg_cb(PurpleAccount *account, const char *who, char *message,
                 PurpleConversation *conv, PurpleMessageFlags flags, gpointer user_data)
{
	if (conv) {
		gif_hook_conversation(conv);
	}
}

static const unsigned char tiny_gif[] = {
	0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0x21, 0xf9, 0x04, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
	0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b
};

static gboolean
plugin_load(PurplePlugin *plugin)
{
	purple_debug_info("gif_loop_limit", "plugin_load called\n");

#ifdef _WIN32
	HMODULE hPidgin = GetModuleHandleA("pidgin.dll");
	if (hPidgin) {
		real_animation_free = (void *)GetProcAddress(hPidgin, "gtk_imhtml_animation_free");
		purple_debug_info("gif_loop_limit", "real_animation_free = %p\n", real_animation_free);
	}
#endif

	/* Force registration of GdkPixbufGifAnimIter type by opening tiny GIF */
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	gdk_pixbuf_loader_write(loader, tiny_gif, sizeof(tiny_gif), NULL);

	GdkPixbufAnimation *anim = gdk_pixbuf_loader_get_animation(loader);
	if (anim) {
		GdkPixbufAnimationIter *iter = gdk_pixbuf_animation_get_iter(anim, NULL);
		if (iter) {
			ensure_hooked(iter);
			g_object_unref(iter);
		}
	}
	gdk_pixbuf_loader_close(loader, NULL);
	g_object_unref(loader);

	GType gif_iter_type = g_type_from_name("GdkPixbufGifAnimIter");
	purple_debug_info("gif_loop_limit", "GdkPixbufGifAnimIter GType = %lu\n", (unsigned long)gif_iter_type);

	if (gif_iter_type != 0 && !hooked_iter_class) {
		hooked_iter_class = (GdkPixbufAnimationIterClass *)g_type_class_ref(gif_iter_type);
		if (hooked_iter_class && hooked_iter_class->advance) {
			orig_advance = hooked_iter_class->advance;
			hooked_iter_class->advance = gif_loop_limit_advance;
			purple_debug_info("gif_loop_limit", "Hooked via g_type_class_ref: class=%p, orig=%p\n",
				hooked_iter_class, orig_advance);
		}
	}

	purple_signal_connect(pidgin_conversations_get_handle(),
		"conversation-displayed", plugin,
		PURPLE_CALLBACK(conversation_displayed_cb), NULL);

	purple_signal_connect(purple_conversations_get_handle(),
		"conversation-created", plugin,
		PURPLE_CALLBACK(conversation_created_cb), NULL);

	purple_signal_connect(pidgin_conversations_get_handle(),
		"displayed-im-msg", plugin,
		PURPLE_CALLBACK(displayed_msg_cb), NULL);

	purple_signal_connect(pidgin_conversations_get_handle(),
		"displayed-chat-msg", plugin,
		PURPLE_CALLBACK(displayed_msg_cb), NULL);

	GList *convs = purple_get_conversations();
	GList *cl;
	for (cl = convs; cl != NULL; cl = cl->next) {
		gif_hook_conversation(cl->data);
	}

	return TRUE;
}

static gboolean
plugin_unload(PurplePlugin *plugin)
{
	purple_debug_info("gif_loop_limit", "plugin_unload called\n");

	if (hooked_iter_class && orig_advance) {
		hooked_iter_class->advance = orig_advance;
		g_type_class_unref(hooked_iter_class);
		hooked_iter_class = NULL;
		orig_advance = NULL;
	}

	purple_signals_disconnect_by_handle(plugin);

	while (active_loop_data_list != NULL) {
		GifLoopData *data = active_loop_data_list->data;
		gif_loop_data_free(data);
	}

	return TRUE;
}

static GtkWidget *
get_config_frame(PurplePlugin *plugin)
{
	GtkWidget *ret;
	GtkWidget *vbox;

	ret = gtk_vbox_new(FALSE, 18);
	gtk_container_set_border_width(GTK_CONTAINER(ret), 12);

	vbox = pidgin_make_frame(ret, _("GIF Loop Limits"));

	pidgin_prefs_labeled_spin_button(vbox, _("Initial number of loops:"),
		PREF_INITIAL_LOOPS, 1, 100, NULL);

	pidgin_prefs_labeled_spin_button(vbox, _("Resume loops on click:"),
		PREF_RESUME_LOOPS, 1, 100, NULL);

	vbox = pidgin_make_frame(ret, _("Visual Appearance"));

	pidgin_prefs_checkbox(_("Show play icon overlay when paused"),
		PREF_SHOW_PLAY_ICON, vbox);

	gtk_widget_show_all(ret);
	return ret;
}

static PidginPluginUiInfo ui_info =
{
	get_config_frame,
	0,
	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	PIDGIN_PLUGIN_TYPE,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,

	PLUGIN_ID,
	PLUGIN_NAME,
	PLUGIN_VERSION,
	PLUGIN_SUMMARY,
	PLUGIN_DESCRIPTION,
	PLUGIN_AUTHOR,
	"https://pidgin.im",

	plugin_load,
	plugin_unload,
	NULL,

	&ui_info,
	NULL,
	NULL,
	NULL,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin(PurplePlugin *plugin)
{
	purple_prefs_add_none(PREFS_PREFIX);
	purple_prefs_add_int(PREF_INITIAL_LOOPS, 3);
	purple_prefs_add_int(PREF_RESUME_LOOPS, 2);
	purple_prefs_add_bool(PREF_SHOW_PLAY_ICON, TRUE);
}

PURPLE_INIT_PLUGIN(PLUGIN_STATIC_NAME, init_plugin, info)
