/* this file is part of evince, a gnome document viewer
 *
 *  Copyright (C) 2008 Carlos Garcia Campos <carlosgc@gnome.org>
 *  Copyright (C) 2005 Red Hat, Inc
 *
 * Evince is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Evince is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include "ev-jobs.h"
#include "ev-document-thumbnails.h"
#include "ev-document-links.h"
#include "ev-document-images.h"
#include "ev-document-forms.h"
#include "ev-file-exporter.h"
#include "ev-document-factory.h"
#include "ev-document-misc.h"
#include "ev-file-helpers.h"
#include "ev-document-fonts.h"
#include "ev-document-security.h"
#include "ev-document-find.h"
#include "ev-document-layers.h"
#include "ev-debug.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <unistd.h>

static void ev_job_init                   (EvJob                 *job);
static void ev_job_class_init             (EvJobClass            *class);
static void ev_job_links_init             (EvJobLinks            *job);
static void ev_job_links_class_init       (EvJobLinksClass       *class);
static void ev_job_attachments_init       (EvJobAttachments      *job);
static void ev_job_attachments_class_init (EvJobAttachmentsClass *class);
static void ev_job_render_init            (EvJobRender           *job);
static void ev_job_render_class_init      (EvJobRenderClass      *class);
static void ev_job_thumbnail_init         (EvJobThumbnail        *job);
static void ev_job_thumbnail_class_init   (EvJobThumbnailClass   *class);
static void ev_job_load_init    	  (EvJobLoad	         *job);
static void ev_job_load_class_init 	  (EvJobLoadClass	 *class);
static void ev_job_save_init              (EvJobSave             *job);
static void ev_job_save_class_init        (EvJobSaveClass        *class);
static void ev_job_find_init              (EvJobFind             *job);
static void ev_job_find_class_init        (EvJobFindClass        *class);
static void ev_job_layers_init            (EvJobLayers           *job);
static void ev_job_layers_class_init      (EvJobLayersClass      *class);
static void ev_job_export_init            (EvJobExport           *job);
static void ev_job_export_class_init      (EvJobExportClass      *class);

enum {
	CANCELLED,
	FINISHED,
	LAST_SIGNAL
};

enum {
	PAGE_READY,
	RENDER_LAST_SIGNAL
};

enum {
	FONTS_UPDATED,
	FONTS_LAST_SIGNAL
};

enum {
	FIND_UPDATED,
	FIND_LAST_SIGNAL
};

static guint job_signals[LAST_SIGNAL] = { 0 };
static guint job_render_signals[RENDER_LAST_SIGNAL] = { 0 };
static guint job_fonts_signals[FONTS_LAST_SIGNAL] = { 0 };
static guint job_find_signals[FIND_LAST_SIGNAL] = { 0 };

G_DEFINE_ABSTRACT_TYPE (EvJob, ev_job, G_TYPE_OBJECT)
G_DEFINE_TYPE (EvJobLinks, ev_job_links, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobAttachments, ev_job_attachments, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobRender, ev_job_render, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobThumbnail, ev_job_thumbnail, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobFonts, ev_job_fonts, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobLoad, ev_job_load, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobSave, ev_job_save, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobFind, ev_job_find, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobLayers, ev_job_layers, EV_TYPE_JOB)
G_DEFINE_TYPE (EvJobExport, ev_job_export, EV_TYPE_JOB)

/* EvJob */
static void
ev_job_init (EvJob *job)
{
	job->cancellable = g_cancellable_new ();
}

static void
ev_job_dispose (GObject *object)
{
	EvJob *job;

	job = EV_JOB (object);

	if (job->document) {
		g_object_unref (job->document);
		job->document = NULL;
	}

	if (job->cancellable) {
		g_object_unref (job->cancellable);
		job->cancellable = NULL;
	}

	if (job->error) {
		g_error_free (job->error);
		job->error = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_parent_class)->dispose) (object);
}

static void
ev_job_class_init (EvJobClass *class)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (class);

	oclass->dispose = ev_job_dispose;

	job_signals[CANCELLED] =
		g_signal_new ("cancelled",
			      EV_TYPE_JOB,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvJobClass, cancelled),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	job_signals [FINISHED] =
		g_signal_new ("finished",
			      EV_TYPE_JOB,
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (EvJobClass, finished),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
}

static gboolean
emit_finished (EvJob *job)
{
	ev_debug_message (DEBUG_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);

	job->idle_finished_id = 0;
	
	if (job->cancelled) {
		ev_debug_message (DEBUG_JOBS, "%s (%p) job was cancelled, do not emit finished", EV_GET_TYPE_NAME (job), job);
	} else {
		ev_profiler_stop (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
		g_signal_emit (job, job_signals[FINISHED], 0);
	}
	
	return FALSE;
}

static void
ev_job_emit_finished (EvJob *job)
{
	ev_debug_message (DEBUG_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);

	if (g_cancellable_is_cancelled (job->cancellable)) {
		ev_debug_message (DEBUG_JOBS, "%s (%p) job was cancelled, returning", EV_GET_TYPE_NAME (job), job);
		return;
	}
	
	job->finished = TRUE;
	
	if (job->run_mode == EV_JOB_RUN_THREAD) {
		job->idle_finished_id =
			g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
					 (GSourceFunc)emit_finished,
					 g_object_ref (job),
					 (GDestroyNotify)g_object_unref);
	} else {
		ev_profiler_stop (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
		g_signal_emit (job, job_signals[FINISHED], 0);
	}
}

gboolean
ev_job_run (EvJob *job)
{
	EvJobClass *class = EV_JOB_GET_CLASS (job);
	
	return class->run (job);
}

void
ev_job_cancel (EvJob *job)
{
	if (job->cancelled || (job->finished && job->idle_finished_id == 0))
		return;

	ev_debug_message (DEBUG_JOBS, "job %s (%p) cancelled", EV_GET_TYPE_NAME (job), job);
	ev_profiler_stop (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	/* This should never be called from a thread */
	job->cancelled = TRUE;
	g_cancellable_cancel (job->cancellable);
	g_signal_emit (job, job_signals[CANCELLED], 0);
}

void
ev_job_failed (EvJob       *job,
	       GQuark       domain,
	       gint         code,
	       const gchar *format,
	       ...)
{
	va_list args;
	gchar  *message;
	
	if (job->failed || job->finished)
		return;

	ev_debug_message (DEBUG_JOBS, "job %s (%p) failed", EV_GET_TYPE_NAME (job), job);
	
	job->failed = TRUE;
	
	va_start (args, format);
	message = g_strdup_vprintf (format, args);
	va_end (args);
	
	job->error = g_error_new_literal (domain, code, message);
	g_free (message);
	
	ev_job_emit_finished (job);                                                                                                               
}

void
ev_job_failed_from_error (EvJob  *job,
			  GError *error)
{
	if (job->failed || job->finished)
		return;
	
	ev_debug_message (DEBUG_JOBS, "job %s (%p) failed", EV_GET_TYPE_NAME (job), job);

	job->failed = TRUE;
	job->error = g_error_copy (error);

	ev_job_emit_finished (job);
}

void
ev_job_succeeded (EvJob *job)
{
	if (job->finished)
		return;

	ev_debug_message (DEBUG_JOBS, "job %s (%p) succeeded", EV_GET_TYPE_NAME (job), job);
	
	job->failed = FALSE;
	ev_job_emit_finished (job);
}

gboolean
ev_job_is_finished (EvJob *job)
{
	return job->finished;
}

gboolean
ev_job_is_failed (EvJob *job)
{
	return job->failed;
}

EvJobRunMode
ev_job_get_run_mode (EvJob *job)
{
	return job->run_mode;
}

void
ev_job_set_run_mode (EvJob       *job,
		     EvJobRunMode run_mode)
{
	job->run_mode = run_mode;
}

/* EvJobLinks */
static void
ev_job_links_init (EvJobLinks *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_links_dispose (GObject *object)
{
	EvJobLinks *job;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = EV_JOB_LINKS (object);

	if (job->model) {
		g_object_unref (job->model);
		job->model = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_links_parent_class)->dispose) (object);
}

static gboolean
ev_job_links_run (EvJob *job)
{
	EvJobLinks *job_links = EV_JOB_LINKS (job);

	ev_debug_message (DEBUG_JOBS, NULL);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();
	job_links->model = ev_document_links_get_links_model (EV_DOCUMENT_LINKS (job->document));
	ev_document_doc_mutex_unlock ();
	
	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_links_class_init (EvJobLinksClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_links_dispose;
	job_class->run = ev_job_links_run;
}

EvJob *
ev_job_links_new (EvDocument *document)
{
	EvJob *job;

	ev_debug_message (DEBUG_JOBS, NULL);

	job = g_object_new (EV_TYPE_JOB_LINKS, NULL);
	job->document = g_object_ref (document);
	
	return job;
}

/* EvJobAttachments */
static void
ev_job_attachments_init (EvJobAttachments *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_attachments_dispose (GObject *object)
{
	EvJobAttachments *job;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = EV_JOB_ATTACHMENTS (object);

	if (job->attachments) {
		g_list_foreach (job->attachments, (GFunc)g_object_unref, NULL);
		g_list_free (job->attachments);
		job->attachments = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_attachments_parent_class)->dispose) (object);
}

static gboolean
ev_job_attachments_run (EvJob *job)
{
	EvJobAttachments *job_attachments = EV_JOB_ATTACHMENTS (job);

	ev_debug_message (DEBUG_JOBS, NULL);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();
	job_attachments->attachments = ev_document_get_attachments (job->document);
	ev_document_doc_mutex_unlock ();
	
	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_attachments_class_init (EvJobAttachmentsClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_attachments_dispose;
	job_class->run = ev_job_attachments_run;
}

EvJob *
ev_job_attachments_new (EvDocument *document)
{
	EvJob *job;

	ev_debug_message (DEBUG_JOBS, NULL);

	job = g_object_new (EV_TYPE_JOB_ATTACHMENTS, NULL);
	job->document = g_object_ref (document);
	
	return job;
}

/* EvJobRender */
static void
ev_job_render_init (EvJobRender *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_render_dispose (GObject *object)
{
	EvJobRender *job;

	job = EV_JOB_RENDER (object);

	if (job->ev_page) {
		ev_debug_message (DEBUG_JOBS, "page: %d (%p)", job->ev_page->index, job);
		g_object_unref (job->ev_page);
		job->ev_page = NULL;
	}
	
	if (job->surface) {
		cairo_surface_destroy (job->surface);
		job->surface = NULL;
	}

	if (job->selection) {
		cairo_surface_destroy (job->selection);
		job->selection = NULL;
	}

	if (job->selection_region) {
		gdk_region_destroy (job->selection_region);
		job->selection_region = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_render_parent_class)->dispose) (object);
}

static gboolean
notify_page_ready (EvJobRender *job)
{
	ev_debug_message (DEBUG_JOBS, "%d (%p)", job->ev_page->index, job);
	ev_profiler_stop (EV_PROFILE_JOBS, "Rendering page %d", job->ev_page->index);

	if (EV_JOB (job)->cancelled) {
		ev_debug_message (DEBUG_JOBS, "%s (%p) job was cancelled, do not emit page_ready", EV_GET_TYPE_NAME (job), job);
	} else {
		g_signal_emit (job, job_render_signals[PAGE_READY], 0);
	}

	return FALSE;
}

static void
ev_job_render_page_ready (EvJobRender *job)
{
	ev_debug_message (DEBUG_JOBS, "%d (%p)", job->ev_page->index, job);
	
	job->page_ready = TRUE;
	g_idle_add_full (G_PRIORITY_HIGH_IDLE,
			 (GSourceFunc)notify_page_ready,
			 g_object_ref (job),
			 (GDestroyNotify)g_object_unref);
}

static gboolean
ev_job_render_run (EvJob *job)
{
	EvJobRender     *job_render = EV_JOB_RENDER (job);
	EvRenderContext *rc;

	ev_debug_message (DEBUG_JOBS, "page: %d (%p)", job_render->page, job);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();

	ev_profiler_start (EV_PROFILE_JOBS, "Rendering page %d", job_render->page);
		
	ev_document_fc_mutex_lock ();

	job_render->ev_page = ev_document_get_page (job->document, job_render->page);
	rc = ev_render_context_new (job_render->ev_page, job_render->rotation, job_render->scale);
		
	job_render->surface = ev_document_render (job->document, rc);
	/* If job was cancelled during the page rendering,
	 * we return now, so that the thread is finished ASAP
	 */
	if (g_cancellable_is_cancelled (job->cancellable)) {
		ev_document_fc_mutex_unlock ();
		ev_document_doc_mutex_unlock ();
		g_object_unref (rc);

		return FALSE;
	}
	
	if ((job_render->flags & EV_RENDER_INCLUDE_SELECTION) && EV_IS_SELECTION (job->document)) {
		ev_selection_render_selection (EV_SELECTION (job->document),
					       rc,
					       &(job_render->selection),
					       &(job_render->selection_points),
					       NULL,
					       job_render->selection_style,
					       &(job_render->text), &(job_render->base));
		job_render->selection_region =
			ev_selection_get_selection_region (EV_SELECTION (job->document),
							   rc,
							   job_render->selection_style,
							   &(job_render->selection_points));
	}

	ev_job_render_page_ready (job_render);
		
	ev_document_fc_mutex_unlock ();
		
	if ((job_render->flags & EV_RENDER_INCLUDE_TEXT) && EV_IS_SELECTION (job->document))
		job_render->text_mapping =
			ev_selection_get_selection_map (EV_SELECTION (job->document), rc);
	if ((job_render->flags & EV_RENDER_INCLUDE_LINKS) && EV_IS_DOCUMENT_LINKS (job->document))
		job_render->link_mapping =
			ev_document_links_get_links (EV_DOCUMENT_LINKS (job->document), job_render->page);
	if ((job_render->flags & EV_RENDER_INCLUDE_FORMS) && EV_IS_DOCUMENT_FORMS (job->document))
		job_render->form_field_mapping =
			ev_document_forms_get_form_fields (EV_DOCUMENT_FORMS (job->document),
							   job_render->ev_page);
	if ((job_render->flags & EV_RENDER_INCLUDE_IMAGES) && EV_IS_DOCUMENT_IMAGES (job->document))
		job_render->image_mapping =
			ev_document_images_get_image_mapping (EV_DOCUMENT_IMAGES (job->document),
							      job_render->page);
	g_object_unref (rc);
	ev_document_doc_mutex_unlock ();
	
	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_render_class_init (EvJobRenderClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	job_render_signals [PAGE_READY] =
		g_signal_new ("page-ready",
			      EV_TYPE_JOB_RENDER,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvJobRenderClass, page_ready),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	oclass->dispose = ev_job_render_dispose;
	job_class->run = ev_job_render_run;
}

EvJob *
ev_job_render_new (EvDocument   *document,
		   gint          page,
		   gint          rotation,
		   gdouble       scale, 
		   gint          width,
		   gint          height,
		   EvRenderFlags flags)
{
	EvJobRender *job;

	ev_debug_message (DEBUG_JOBS, "page: %d", page);
	
	job = g_object_new (EV_TYPE_JOB_RENDER, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->page = page;
	job->rotation = rotation;
	job->scale = scale;
	job->target_width = width;
	job->target_height = height;
	job->flags = flags;

	return EV_JOB (job);
}

void
ev_job_render_set_selection_info (EvJobRender     *job,
				  EvRectangle     *selection_points,
				  EvSelectionStyle selection_style,
				  GdkColor        *text,
				  GdkColor        *base)
{
	job->flags |= EV_RENDER_INCLUDE_SELECTION;
	
	job->selection_points = *selection_points;
	job->selection_style = selection_style;
	job->text = *text;
	job->base = *base;
}

/* EvJobThumbnail */
static void
ev_job_thumbnail_init (EvJobThumbnail *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_thumbnail_dispose (GObject *object)
{
	EvJobThumbnail *job;

	job = EV_JOB_THUMBNAIL (object);

	ev_debug_message (DEBUG_JOBS, "%d (%p)", job->page, job);
	
	if (job->thumbnail) {
		g_object_unref (job->thumbnail);
		job->thumbnail = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_thumbnail_parent_class)->dispose) (object);
}

static gboolean
ev_job_thumbnail_run (EvJob *job)
{
	EvJobThumbnail  *job_thumb = EV_JOB_THUMBNAIL (job);
	EvRenderContext *rc;
	EvPage          *page;

	ev_debug_message (DEBUG_JOBS, "%d (%p)", job_thumb->page, job);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();

	page = ev_document_get_page (job->document, job_thumb->page);
	rc = ev_render_context_new (page, job_thumb->rotation, job_thumb->scale);
	g_object_unref (page);

	job_thumb->thumbnail = ev_document_thumbnails_get_thumbnail (EV_DOCUMENT_THUMBNAILS (job->document),
								     rc, TRUE);
	g_object_unref (rc);
	ev_document_doc_mutex_unlock ();

	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_thumbnail_class_init (EvJobThumbnailClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_thumbnail_dispose;
	job_class->run = ev_job_thumbnail_run;
}

EvJob *
ev_job_thumbnail_new (EvDocument *document,
		      gint        page,
		      gint        rotation,
		      gdouble     scale)
{
	EvJobThumbnail *job;

	ev_debug_message (DEBUG_JOBS, "%d", page);
	
	job = g_object_new (EV_TYPE_JOB_THUMBNAIL, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->page = page;
	job->rotation = rotation;
	job->scale = scale;

	return EV_JOB (job);
}

/* EvJobFonts */
static void
ev_job_fonts_init (EvJobFonts *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_MAIN_LOOP;
}

static gboolean
ev_job_fonts_run (EvJob *job)
{
	EvJobFonts      *job_fonts = EV_JOB_FONTS (job);
	EvDocumentFonts *fonts = EV_DOCUMENT_FONTS (job->document);

	ev_debug_message (DEBUG_JOBS, NULL);
	
	/* Do not block the main loop */
	if (!ev_document_doc_mutex_trylock ())
		return TRUE;
	
	if (!ev_document_fc_mutex_trylock ())
		return TRUE;

#ifdef EV_ENABLE_DEBUG
	/* We use the #ifdef in this case because of the if */
	if (ev_document_fonts_get_progress (fonts) == 0)
		ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
#endif

	job_fonts->scan_completed = !ev_document_fonts_scan (fonts, 20);
	g_signal_emit (job_fonts, job_fonts_signals[FONTS_UPDATED], 0,
		       ev_document_fonts_get_progress (fonts));

	ev_document_fc_mutex_unlock ();
	ev_document_doc_mutex_unlock ();

	if (job_fonts->scan_completed)
		ev_job_succeeded (job);
	
	return !job_fonts->scan_completed;
}

static void
ev_job_fonts_class_init (EvJobFontsClass *class)
{
	EvJobClass *job_class = EV_JOB_CLASS (class);
	
	job_class->run = ev_job_fonts_run;
	
	job_fonts_signals[FONTS_UPDATED] =
		g_signal_new ("updated",
			      EV_TYPE_JOB_FONTS,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvJobFontsClass, updated),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__DOUBLE,
			      G_TYPE_NONE,
			      1, G_TYPE_DOUBLE);
}

EvJob *
ev_job_fonts_new (EvDocument *document)
{
	EvJobFonts *job;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = g_object_new (EV_TYPE_JOB_FONTS, NULL);

	EV_JOB (job)->document = g_object_ref (document);

	return EV_JOB (job);
}

/* EvJobLoad */
static void
ev_job_load_init (EvJobLoad *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_load_dispose (GObject *object)
{
	EvJobLoad *job = EV_JOB_LOAD (object);

	ev_debug_message (DEBUG_JOBS, "%s", job->uri);
	
	if (job->uri) {
		g_free (job->uri);
		job->uri = NULL;
	}

	if (job->password) {
		g_free (job->password);
		job->password = NULL;
	}

	if (job->dest) {
		g_object_unref (job->dest);
		job->dest = NULL;
	}

	if (job->search_string) {
		g_free (job->search_string);
		job->search_string = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_load_parent_class)->dispose) (object);
}

static gboolean
ev_job_load_run (EvJob *job)
{
	EvJobLoad *job_load = EV_JOB_LOAD (job);
	GError    *error = NULL;
	
	ev_debug_message (DEBUG_JOBS, "%s", job_load->uri);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_fc_mutex_lock ();

	/* This job may already have a document even if the job didn't complete
	   because, e.g., a password is required - if so, just reload rather than
	   creating a new instance */
	if (job->document) {
		if (job_load->password) {
			ev_document_security_set_password (EV_DOCUMENT_SECURITY (job->document),
							   job_load->password);
		}
		
		job->failed = FALSE;
		job->finished = FALSE;
		g_clear_error (&job->error);
		
		ev_document_load (job->document,
				  job_load->uri,
				  &error);
	} else {
		job->document = ev_document_factory_get_document (job_load->uri,
								  &error);
	}

	ev_document_fc_mutex_unlock ();

	if (error) {
		ev_job_failed_from_error (job, error);
		g_error_free (error);
	} else {
		ev_job_succeeded (job);
	}

	return FALSE;
}

static void
ev_job_load_class_init (EvJobLoadClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_load_dispose;
	job_class->run = ev_job_load_run;
}

EvJob *
ev_job_load_new (const gchar    *uri,
		 EvLinkDest     *dest,
		 EvWindowRunMode mode,
		 const gchar    *search_string)
{
	EvJobLoad *job;

	ev_debug_message (DEBUG_JOBS, "%s", uri);
	
	job = g_object_new (EV_TYPE_JOB_LOAD, NULL);

	job->uri = g_strdup (uri);
	job->dest = dest ? g_object_ref (dest) : NULL;
	job->mode = mode;
	job->search_string = search_string ? g_strdup (search_string) : NULL;

	return EV_JOB (job);
}

void
ev_job_load_set_uri (EvJobLoad *job, const gchar *uri)
{
	ev_debug_message (DEBUG_JOBS, "%s", uri);
	
	if (job->uri)
		g_free (job->uri);
	job->uri = g_strdup (uri);
}

void
ev_job_load_set_password (EvJobLoad *job, const gchar *password)
{
	ev_debug_message (DEBUG_JOBS, NULL);

	if (job->password)
		g_free (job->password);
	job->password = password ? g_strdup (password) : NULL;
}

/* EvJobSave */
static void
ev_job_save_init (EvJobSave *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_save_dispose (GObject *object)
{
	EvJobSave *job = EV_JOB_SAVE (object);

	ev_debug_message (DEBUG_JOBS, "%s", job->uri);
	
	if (job->uri) {
		g_free (job->uri);
		job->uri = NULL;
	}

	if (job->document_uri) {
		g_free (job->document_uri);
		job->document_uri = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_save_parent_class)->dispose) (object);
}

static gboolean
ev_job_save_run (EvJob *job)
{
	EvJobSave *job_save = EV_JOB_SAVE (job);
	gint       fd;
	gchar     *filename;
	gchar     *tmp_filename;
	gchar     *local_uri;
	GError    *error = NULL;
	
	ev_debug_message (DEBUG_JOBS, "uri: %s, document_uri: %s", job_save->uri, job_save->document_uri);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	filename = ev_tmp_filename ("saveacopy");
	tmp_filename = g_strdup_printf ("%s.XXXXXX", filename);
	g_free (filename);

	fd = g_mkstemp (tmp_filename);
	if (fd == -1) {
		gchar *display_name;
		gint   save_errno = errno;

		display_name = g_filename_display_name (tmp_filename);
		ev_job_failed (job,
			       G_FILE_ERROR,
			       g_file_error_from_errno (save_errno),
			       _("Failed to create file “%s”: %s"),
			       display_name, g_strerror (save_errno));
		g_free (display_name);
		g_free (tmp_filename);

		return FALSE;
	}

	ev_document_doc_mutex_lock ();

	/* Save document to temp filename */
	local_uri = g_filename_to_uri (tmp_filename, NULL, NULL);
	ev_document_save (job->document, local_uri, &error);
	close (fd);

	ev_document_doc_mutex_unlock ();

	if (error) {
		g_free (local_uri);
		ev_job_failed_from_error (job, error);
		g_error_free (error);
		
		return FALSE;
	}

	/* If original document was compressed,
	 * compress it again before saving
	 */
	if (g_object_get_data (G_OBJECT (job->document), "uri-uncompressed")) {
		EvCompressionType ctype = EV_COMPRESSION_NONE;
		const gchar      *ext;
		gchar            *uri_comp;
		
		ext = g_strrstr (job_save->document_uri, ".gz");
		if (ext && g_ascii_strcasecmp (ext, ".gz") == 0)
			ctype = EV_COMPRESSION_GZIP;
		
		ext = g_strrstr (job_save->document_uri, ".bz2");
		if (ext && g_ascii_strcasecmp (ext, ".bz2") == 0)
			ctype = EV_COMPRESSION_BZIP2;

		uri_comp = ev_file_compress (local_uri, ctype, &error);
		g_free (local_uri);
		ev_tmp_filename_unlink (tmp_filename);

		if (!uri_comp || error) {
			local_uri = NULL;
		} else {
			local_uri = uri_comp;
		}
	}

	g_free (tmp_filename);
	
	if (error) {
		g_free (local_uri);
		ev_job_failed_from_error (job, error);
		g_error_free (error);
		
		return FALSE;
	}

	if (!local_uri)
		return FALSE;

	ev_xfer_uri_simple (local_uri, job_save->uri, &error);
	ev_tmp_uri_unlink (local_uri);

	if (error) {
		ev_job_failed_from_error (job, error);
		g_error_free (error);
	} else {
		ev_job_succeeded (job);
	}
	
	return FALSE;
}

static void
ev_job_save_class_init (EvJobSaveClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_save_dispose;
	job_class->run = ev_job_save_run;
}

EvJob *
ev_job_save_new (EvDocument  *document,
		 const gchar *uri,
		 const gchar *document_uri)
{
	EvJobSave *job;

	ev_debug_message (DEBUG_JOBS, "uri: %s, document_uri: %s", uri, document_uri);

	job = g_object_new (EV_TYPE_JOB_SAVE, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->uri = g_strdup (uri);
	job->document_uri = g_strdup (document_uri);

	return EV_JOB (job);
}

/* EvJobFind */
static void
ev_job_find_init (EvJobFind *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_MAIN_LOOP;
}

static void
ev_job_find_dispose (GObject *object)
{
	EvJobFind *job = EV_JOB_FIND (object);

	ev_debug_message (DEBUG_JOBS, NULL);

	if (job->text) {
		g_free (job->text);
		job->text = NULL;
	}

	if (job->pages) {
		gint i;

		for (i = 0; i < job->n_pages; i++) {
			g_list_foreach (job->pages[i], (GFunc)g_free, NULL);
			g_list_free (job->pages[i]);
		}

		g_free (job->pages);
		job->pages = NULL;
	}
	
	(* G_OBJECT_CLASS (ev_job_find_parent_class)->dispose) (object);
}

static gboolean
ev_job_find_run (EvJob *job)
{
	EvJobFind      *job_find = EV_JOB_FIND (job);
	EvDocumentFind *find = EV_DOCUMENT_FIND (job->document);
	EvPage         *ev_page;
	GList          *matches;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	/* Do not block the main loop */
	if (!ev_document_doc_mutex_trylock ())
		return TRUE;
	
#ifdef EV_ENABLE_DEBUG
	/* We use the #ifdef in this case because of the if */
	if (job_find->current_page == job_find->start_page)
		ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
#endif

	ev_page = ev_document_get_page (job->document, job_find->current_page);
	matches = ev_document_find_find_text (find, ev_page, job_find->text,
					      job_find->case_sensitive);
	g_object_unref (ev_page);
	
	ev_document_doc_mutex_unlock ();

	if (!job_find->has_results)
		job_find->has_results = (matches != NULL);

	job_find->pages[job_find->current_page] = matches;
	g_signal_emit (job_find, job_find_signals[FIND_UPDATED], 0, job_find->current_page);
		       
	job_find->current_page = (job_find->current_page + 1) % job_find->n_pages;
	if (job_find->current_page == job_find->start_page) {
		ev_job_succeeded (job);

		return FALSE;
	}

	return TRUE;
}

static void
ev_job_find_class_init (EvJobFindClass *class)
{
	EvJobClass   *job_class = EV_JOB_CLASS (class);
	GObjectClass *gobject_class = G_OBJECT_CLASS (class);
	
	job_class->run = ev_job_find_run;
	gobject_class->dispose = ev_job_find_dispose;
	
	job_find_signals[FIND_UPDATED] =
		g_signal_new ("updated",
			      EV_TYPE_JOB_FIND,
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (EvJobFindClass, updated),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE,
			      1, G_TYPE_INT);
}

EvJob *
ev_job_find_new (EvDocument  *document,
		 gint         start_page,
		 gint         n_pages,
		 const gchar *text,
		 gboolean     case_sensitive)
{
	EvJobFind *job;
	
	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = g_object_new (EV_TYPE_JOB_FIND, NULL);

	EV_JOB (job)->document = g_object_ref (document);
	job->start_page = start_page;
	job->current_page = start_page;
	job->n_pages = n_pages;
	job->pages = g_new0 (GList *, n_pages);
	job->text = g_strdup (text);
	job->case_sensitive = case_sensitive;
	job->has_results = FALSE;

	return EV_JOB (job);
}

gint
ev_job_find_get_n_results (EvJobFind *job,
			   gint       page)
{
	return g_list_length (job->pages[page]);
}

gdouble
ev_job_find_get_progress (EvJobFind *job)
{
	gint pages_done;

	if (ev_job_is_finished (EV_JOB (job)))
		return 1.0;
	
	if (job->current_page > job->start_page) {
		pages_done = job->current_page - job->start_page + 1;
	} else if (job->current_page == job->start_page) {
		pages_done = job->n_pages;
	} else {
		pages_done = job->n_pages - job->start_page + job->current_page;
	}

	return pages_done / (gdouble) job->n_pages;
}

gboolean
ev_job_find_has_results (EvJobFind *job)
{
	return job->has_results;
}

GList **
ev_job_find_get_results (EvJobFind *job)
{
	return job->pages;
}

/* EvJobLayers */
static void
ev_job_layers_init (EvJobLayers *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
}

static void
ev_job_layers_dispose (GObject *object)
{
	EvJobLayers *job;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = EV_JOB_LAYERS (object);

	if (job->model) {
		g_object_unref (job->model);
		job->model = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_layers_parent_class)->dispose) (object);
}

static gboolean
ev_job_layers_run (EvJob *job)
{
	EvJobLayers *job_layers = EV_JOB_LAYERS (job);

	ev_debug_message (DEBUG_JOBS, NULL);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();
	job_layers->model = ev_document_layers_get_layers (EV_DOCUMENT_LAYERS (job->document));
	ev_document_doc_mutex_unlock ();
	
	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_layers_class_init (EvJobLayersClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_layers_dispose;
	job_class->run = ev_job_layers_run;
}

EvJob *
ev_job_layers_new (EvDocument *document)
{
	EvJob *job;

	ev_debug_message (DEBUG_JOBS, NULL);

	job = g_object_new (EV_TYPE_JOB_LAYERS, NULL);
	job->document = g_object_ref (document);
	
	return job;
}

/* EvJobExport */
static void
ev_job_export_init (EvJobExport *job)
{
	EV_JOB (job)->run_mode = EV_JOB_RUN_THREAD;
	job->page = -1;
}

static void
ev_job_export_dispose (GObject *object)
{
	EvJobExport *job;

	ev_debug_message (DEBUG_JOBS, NULL);
	
	job = EV_JOB_EXPORT (object);

	if (job->rc) {
		g_object_unref (job->rc);
		job->rc = NULL;
	}

	(* G_OBJECT_CLASS (ev_job_export_parent_class)->dispose) (object);
}

static gboolean
ev_job_export_run (EvJob *job)
{
	EvJobExport *job_export = EV_JOB_EXPORT (job);
	EvPage      *ev_page;

	g_assert (job_export->page != -1);

	ev_debug_message (DEBUG_JOBS, NULL);
	ev_profiler_start (EV_PROFILE_JOBS, "%s (%p)", EV_GET_TYPE_NAME (job), job);
	
	ev_document_doc_mutex_lock ();
	
	ev_page = ev_document_get_page (job->document, job_export->page);
	if (job_export->rc) {
		job->failed = FALSE;
		job->finished = FALSE;
		g_clear_error (&job->error);
		
		ev_render_context_set_page (job_export->rc, ev_page);
	} else {
		job_export->rc = ev_render_context_new (ev_page, 0, 1.0);
	}
	g_object_unref (ev_page);
	
	ev_file_exporter_do_page (EV_FILE_EXPORTER (job->document), job_export->rc);
	
	ev_document_doc_mutex_unlock ();
	
	ev_job_succeeded (job);
	
	return FALSE;
}

static void
ev_job_export_class_init (EvJobExportClass *class)
{
	GObjectClass *oclass = G_OBJECT_CLASS (class);
	EvJobClass   *job_class = EV_JOB_CLASS (class);

	oclass->dispose = ev_job_export_dispose;
	job_class->run = ev_job_export_run;
}

EvJob *
ev_job_export_new (EvDocument *document)
{
	EvJob *job;

	ev_debug_message (DEBUG_JOBS, NULL);

	job = g_object_new (EV_TYPE_JOB_EXPORT, NULL);
	job->document = g_object_ref (document);
	
	return job;
}

void
ev_job_export_set_page (EvJobExport *job,
			gint         page)
{
	job->page = page;
}
