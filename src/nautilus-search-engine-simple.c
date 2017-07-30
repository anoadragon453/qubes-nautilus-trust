/*
 * Copyright (C) 2005 Red Hat, Inc
 *
 * Nautilus is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nautilus is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; see the file COPYING.  If not,
 * see <http://www.gnu.org/licenses/>.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 *
 */

#include <config.h>
#include "nautilus-search-hit.h"
#include "nautilus-search-provider.h"
#include "nautilus-search-engine-simple.h"
#include "nautilus-ui-utilities.h"
#define DEBUG_FLAG NAUTILUS_DEBUG_SEARCH
#include "nautilus-debug.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

#define BATCH_SIZE 500

enum
{
    PROP_RECURSIVE = 1,
    PROP_RUNNING,
    NUM_PROPERTIES
};

typedef struct
{
    NautilusSearchEngineSimple *engine;
    GCancellable *cancellable;

    GList *mime_types;
    GList *found_list;

    GQueue *directories;     /* GFiles */

    GHashTable *visited;

    gboolean recursive;
    gint n_processed_files;
    GList *hits;

    NautilusQuery *query;
} SearchThreadData;


struct _NautilusSearchEngineSimple
{
    GObject parent_instance;
    NautilusQuery *query;

    SearchThreadData *active_search;

    gboolean recursive;
};

static void nautilus_search_provider_init (NautilusSearchProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE (NautilusSearchEngineSimple,
                         nautilus_search_engine_simple,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (NAUTILUS_TYPE_SEARCH_PROVIDER,
                                                nautilus_search_provider_init))

static void
finalize (GObject *object)
{
    NautilusSearchEngineSimple *simple = NAUTILUS_SEARCH_ENGINE_SIMPLE (object);
    g_clear_object (&simple->query);

    G_OBJECT_CLASS (nautilus_search_engine_simple_parent_class)->finalize (object);
}

static SearchThreadData *
search_thread_data_new (NautilusSearchEngineSimple *engine,
                        NautilusQuery              *query)
{
    SearchThreadData *data;
    GFile *location;

    data = g_new0 (SearchThreadData, 1);

    data->engine = g_object_ref (engine);
    data->directories = g_queue_new ();
    data->visited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    data->query = g_object_ref (query);

    location = nautilus_query_get_location (query);

    g_queue_push_tail (data->directories, location);
    data->mime_types = nautilus_query_get_mime_types (query);

    data->cancellable = g_cancellable_new ();

    return data;
}

static void
search_thread_data_free (SearchThreadData *data)
{
    g_queue_foreach (data->directories,
                     (GFunc) g_object_unref, NULL);
    g_queue_free (data->directories);
    g_hash_table_destroy (data->visited);
    g_object_unref (data->cancellable);
    g_object_unref (data->query);
    g_list_free_full (data->mime_types, g_free);
    g_list_free_full (data->hits, g_object_unref);
    g_object_unref (data->engine);

    g_free (data);
}

static gboolean
search_thread_done_idle (gpointer user_data)
{
    SearchThreadData *data = user_data;
    NautilusSearchEngineSimple *engine = data->engine;

    if (g_cancellable_is_cancelled (data->cancellable))
    {
        DEBUG ("Simple engine finished and cancelled");
    }
    else
    {
        DEBUG ("Simple engine finished");
    }
    engine->active_search = NULL;
    nautilus_search_provider_finished (NAUTILUS_SEARCH_PROVIDER (engine),
                                       NAUTILUS_SEARCH_PROVIDER_STATUS_NORMAL);

    g_object_notify (G_OBJECT (engine), "running");

    search_thread_data_free (data);

    return FALSE;
}

typedef struct
{
    GList *hits;
    SearchThreadData *thread_data;
} SearchHitsData;


static gboolean
search_thread_add_hits_idle (gpointer user_data)
{
    SearchHitsData *data = user_data;

    if (!g_cancellable_is_cancelled (data->thread_data->cancellable))
    {
        DEBUG ("Simple engine add hits");
        nautilus_search_provider_hits_added (NAUTILUS_SEARCH_PROVIDER (data->thread_data->engine),
                                             data->hits);
    }

    g_list_free_full (data->hits, g_object_unref);
    g_free (data);

    return FALSE;
}

static void
send_batch (SearchThreadData *thread_data)
{
    SearchHitsData *data;

    thread_data->n_processed_files = 0;

    if (thread_data->hits)
    {
        data = g_new (SearchHitsData, 1);
        data->hits = thread_data->hits;
        data->thread_data = thread_data;
        g_idle_add (search_thread_add_hits_idle, data);
    }
    thread_data->hits = NULL;
}

#define STD_ATTRIBUTES \
    G_FILE_ATTRIBUTE_STANDARD_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP "," \
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," \
    G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," \
    G_FILE_ATTRIBUTE_TIME_ACCESS "," \
    G_FILE_ATTRIBUTE_ID_FILE

static void
visit_directory (GFile            *dir,
                 SearchThreadData *data)
{
    GFileEnumerator *enumerator;
    GFileInfo *info;
    GFile *child;
    const char *mime_type, *display_name;
    gdouble match;
    gboolean is_hidden, found;
    GList *l;
    const char *id;
    gboolean visited;
    guint64 atime;
    guint64 mtime;
    GPtrArray *date_range;
    GDateTime *initial_date;
    GDateTime *end_date;


    enumerator = g_file_enumerate_children (dir,
                                            data->mime_types != NULL ?
                                            STD_ATTRIBUTES ","
                                            G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
                                            :
                                            STD_ATTRIBUTES
                                            ,
                                            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                            data->cancellable, NULL);

    if (enumerator == NULL)
    {
        return;
    }

    while ((info = g_file_enumerator_next_file (enumerator, data->cancellable, NULL)) != NULL)
    {
        display_name = g_file_info_get_display_name (info);
        if (display_name == NULL)
        {
            goto next;
        }

        is_hidden = g_file_info_get_is_hidden (info) || g_file_info_get_is_backup (info);
        if (is_hidden && !nautilus_query_get_show_hidden_files (data->query))
        {
            goto next;
        }

        child = g_file_get_child (dir, g_file_info_get_name (info));
        match = nautilus_query_matches_string (data->query, display_name);
        found = (match > -1);

        if (found && data->mime_types)
        {
            mime_type = g_file_info_get_content_type (info);
            found = FALSE;

            for (l = data->mime_types; mime_type != NULL && l != NULL; l = l->next)
            {
                if (g_content_type_is_a (mime_type, l->data))
                {
                    found = TRUE;
                    break;
                }
            }
        }

        mtime = g_file_info_get_attribute_uint64 (info, "time::modified");
        atime = g_file_info_get_attribute_uint64 (info, "time::access");

        date_range = nautilus_query_get_date_range (data->query);
        if (found && date_range != NULL)
        {
            NautilusQuerySearchType type;
            guint64 current_file_time;

            initial_date = g_ptr_array_index (date_range, 0);
            end_date = g_ptr_array_index (date_range, 1);
            type = nautilus_query_get_search_type (data->query);

            if (type == NAUTILUS_QUERY_SEARCH_TYPE_LAST_ACCESS)
            {
                current_file_time = atime;
            }
            else
            {
                current_file_time = mtime;
            }
            found = nautilus_file_date_in_between (current_file_time,
                                                   initial_date,
                                                   end_date);
            g_ptr_array_unref (date_range);
        }

        if (found)
        {
            NautilusSearchHit *hit;
            GDateTime *date;
            char *uri;

            uri = g_file_get_uri (child);
            hit = nautilus_search_hit_new (uri);
            g_free (uri);
            nautilus_search_hit_set_fts_rank (hit, match);
            date = g_date_time_new_from_unix_local (mtime);
            nautilus_search_hit_set_modification_time (hit, date);
            g_date_time_unref (date);

            data->hits = g_list_prepend (data->hits, hit);
        }

        data->n_processed_files++;
        if (data->n_processed_files > BATCH_SIZE)
        {
            send_batch (data);
        }

        if (data->engine->recursive && g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
            id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE);
            visited = FALSE;
            if (id)
            {
                if (g_hash_table_lookup_extended (data->visited,
                                                  id, NULL, NULL))
                {
                    visited = TRUE;
                }
                else
                {
                    g_hash_table_insert (data->visited, g_strdup (id), NULL);
                }
            }

            if (!visited)
            {
                g_queue_push_tail (data->directories, g_object_ref (child));
            }
        }

        g_object_unref (child);
next:
        g_object_unref (info);
    }

    g_object_unref (enumerator);
}


static gpointer
search_thread_func (gpointer user_data)
{
    SearchThreadData *data;
    GFile *dir;
    GFileInfo *info;
    const char *id;

    data = user_data;

    /* Insert id for toplevel directory into visited */
    dir = g_queue_peek_head (data->directories);
    info = g_file_query_info (dir, G_FILE_ATTRIBUTE_ID_FILE, 0, data->cancellable, NULL);
    if (info)
    {
        id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE);
        if (id)
        {
            g_hash_table_insert (data->visited, g_strdup (id), NULL);
        }
        g_object_unref (info);
    }

    while (!g_cancellable_is_cancelled (data->cancellable) &&
           (dir = g_queue_pop_head (data->directories)) != NULL)
    {
        visit_directory (dir, data);
        g_object_unref (dir);
    }

    if (!g_cancellable_is_cancelled (data->cancellable))
    {
        send_batch (data);
    }

    g_idle_add (search_thread_done_idle, data);

    return NULL;
}

static void
nautilus_search_engine_simple_start (NautilusSearchProvider *provider)
{
    NautilusSearchEngineSimple *simple;
    SearchThreadData *data;
    GThread *thread;

    simple = NAUTILUS_SEARCH_ENGINE_SIMPLE (provider);

    if (simple->active_search != NULL)
    {
        return;
    }

    DEBUG ("Simple engine start");

    data = search_thread_data_new (simple, simple->query);

    thread = g_thread_new ("nautilus-search-simple", search_thread_func, data);
    simple->active_search = data;

    g_object_notify (G_OBJECT (provider), "running");

    g_thread_unref (thread);
}

static void
nautilus_search_engine_simple_stop (NautilusSearchProvider *provider)
{
    NautilusSearchEngineSimple *simple = NAUTILUS_SEARCH_ENGINE_SIMPLE (provider);

    if (simple->active_search != NULL)
    {
        DEBUG ("Simple engine stop");
        g_cancellable_cancel (simple->active_search->cancellable);
    }
}

static void
nautilus_search_engine_simple_set_query (NautilusSearchProvider *provider,
                                         NautilusQuery          *query)
{
    NautilusSearchEngineSimple *simple = NAUTILUS_SEARCH_ENGINE_SIMPLE (provider);

    g_object_ref (query);
    g_clear_object (&simple->query);
    simple->query = query;
}

static gboolean
nautilus_search_engine_simple_is_running (NautilusSearchProvider *provider)
{
    NautilusSearchEngineSimple *simple;

    simple = NAUTILUS_SEARCH_ENGINE_SIMPLE (provider);

    return simple->active_search != NULL;
}

static void
nautilus_search_engine_simple_set_property (GObject      *object,
                                            guint         arg_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
    NautilusSearchEngineSimple *engine = NAUTILUS_SEARCH_ENGINE_SIMPLE (object);

    switch (arg_id)
    {
        case PROP_RECURSIVE:
        {
            engine->recursive = g_value_get_boolean (value);
        }
        break;

        default:
        {
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, arg_id, pspec);
        }
        break;
    }
}

static void
nautilus_search_engine_simple_get_property (GObject    *object,
                                            guint       arg_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
    NautilusSearchEngineSimple *engine = NAUTILUS_SEARCH_ENGINE_SIMPLE (object);

    switch (arg_id)
    {
        case PROP_RUNNING:
        {
            g_value_set_boolean (value, nautilus_search_engine_simple_is_running (NAUTILUS_SEARCH_PROVIDER (engine)));
        }
        break;

        case PROP_RECURSIVE:
        {
            g_value_set_boolean (value, engine->recursive);
        }
        break;
    }
}

static void
nautilus_search_provider_init (NautilusSearchProviderInterface *iface)
{
    iface->set_query = nautilus_search_engine_simple_set_query;
    iface->start = nautilus_search_engine_simple_start;
    iface->stop = nautilus_search_engine_simple_stop;
    iface->is_running = nautilus_search_engine_simple_is_running;
}

static void
nautilus_search_engine_simple_class_init (NautilusSearchEngineSimpleClass *class)
{
    GObjectClass *gobject_class;

    gobject_class = G_OBJECT_CLASS (class);
    gobject_class->finalize = finalize;
    gobject_class->get_property = nautilus_search_engine_simple_get_property;
    gobject_class->set_property = nautilus_search_engine_simple_set_property;

    /**
     * NautilusSearchEngineSimple::recursive:
     *
     * Whether the search is recursive or not.
     */
    g_object_class_install_property (gobject_class,
                                     PROP_RECURSIVE,
                                     g_param_spec_boolean ("recursive",
                                                           "recursive",
                                                           "recursive",
                                                           FALSE,
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

    /**
     * NautilusSearchEngine::running:
     *
     * Whether the search engine is running a search.
     */
    g_object_class_override_property (gobject_class, PROP_RUNNING, "running");
}

static void
nautilus_search_engine_simple_init (NautilusSearchEngineSimple *engine)
{
    engine->query = NULL;
    engine->active_search = NULL;
}

NautilusSearchEngineSimple *
nautilus_search_engine_simple_new (void)
{
    NautilusSearchEngineSimple *engine;

    engine = g_object_new (NAUTILUS_TYPE_SEARCH_ENGINE_SIMPLE, NULL);

    return engine;
}