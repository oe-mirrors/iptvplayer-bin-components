/*
 * Copyright (C) 2008 Felipe Contreras.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
Based on:
- https://github.com/sreerenjb/gstplayer/blob/master/gst-backend.c
- https://github.com/OpenViX/enigma2/blob/master/lib/service/servicemp3.cpp
*/

#include "gst-backend.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <glib-object.h>
#include <sys/time.h>
#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>

static GstElement *g_gst_playbin = NULL;
static GstElement *g_dvbAudioSink   = NULL;
static GstElement *g_dvbVideoSink   = NULL;
static GstElement *g_subsink = NULL;
static GstElement *g_gstIFDSrc   = NULL;
static GstSeekFlags g_seek_flags  = GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT;
#ifdef PLATFORM_I686
static GstPlayFlags g_playbin_flags = ( GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO );
#else
static GstPlayFlags g_playbin_flags = ( GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO | GST_PLAY_FLAG_NATIVE_VIDEO); // 0x47 - GST_PLAY_FLAG_TEXT;
#endif

static PlaybackInfo_t g_playback_info;

static gboolean g_is_local_file = 0;
static gint64  g_duration = 0;
static gchar  *g_filename = NULL;
static gchar  *g_download_buffer_path = NULL;
static guint64 g_ring_buffer_max_size = -0;
static gint64  g_buffer_duration = -1;
static gint    g_buffer_size = 0;
static StrPair_t **g_ptr_http_header_fields = NULL;
static int  g_sfd = -1;     /* Used to wake up main loop when new message in the gst bus is available */
static int g_fileFd = -1;
static guint64 g_iptv_download_timeout = 0;
static gboolean g_is_live = TRUE;

static struct
{
    gint64 check_timestamp;
    gint64 decoder_time;
    gint64 playbin_time;
} g_eos_fix;

/* Include common functions */
#include "tracks.h"

/* return monotonic timestamp in miliseconds (the wall clock jumps when the
 * box syncs its time, which made the EOS fix stop playback) */
static gint64 getTimestamp()
{
    return g_get_monotonic_time() / 1000;
}

gchar *json_escape(const gchar *str)
{
    const guchar *p = NULL;
    GString *out = NULL;

    if (NULL == str)
    {
        return g_strdup("");
    }

    out = g_string_sized_new(strlen(str) + 8);
    for (p = (const guchar *) str; *p; ++p)
    {
        switch (*p)
        {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if (*p < 0x20)
                {
                    g_string_append_printf(out, "\\u%04x", *p);
                }
                else
                {
                    g_string_append_c(out, *p);
                }
                break;
        }
    }
    return g_string_free(out, FALSE);
}

static gint match_sinktype(const GValue *velement, const gchar *type)
{
    GstElement *element = GST_ELEMENT_CAST(g_value_get_object(velement));
    return strcmp(g_type_name(G_OBJECT_TYPE(element)), type);
}

static void InfoStructChanged()
{
    PlaybackInfo_t *ptrP = &g_playback_info;

    fprintf(stderr, "{\"PLAYBACK_INFO\":{ \"isPlaying\":%s, \"isPaused\":%s, \"isForwarding\":%s, \"isSeeking\":%s, \"isCreationPhase\":%s,", \
    DUMP_BOOL(ptrP->isPlaying), DUMP_BOOL(ptrP->isPaused), DUMP_BOOL(ptrP->isForwarding), DUMP_BOOL(ptrP->isSeeking), DUMP_BOOL(ptrP->isCreationPhase) );
    fprintf(stderr, "\"BackWard\":%f, \"SlowMotion\":%d, \"Speed\":%d, \"AVSync\":%d,", ptrP->BackWard, ptrP->SlowMotion, ptrP->Speed, ptrP->AVSync);
    fprintf(stderr, " \"isVideo\":%s, \"isAudio\":%s, \"isSubtitle\":%s }}\n", \
    DUMP_BOOL(ptrP->isVideo), DUMP_BOOL(ptrP->isAudio), DUMP_BOOL(ptrP->isSubtitle) );
    /* fprintf(stderr, "{\"PLAYBACK_BUFFERING\":{\"percent\":%d}}\n", percent); g_playback_info.BufferingPercent */
}

static int ChangeSpeed(const gboolean result, const float fSpeed)
{
    if(result)
    {
        g_playback_info.isForwarding = 0;
        g_playback_info.BackWard     = 0;
        g_playback_info.SlowMotion   = 0;
        g_playback_info.Speed        = 1;

        int iSpeed = (int)fSpeed;
        int iRSpeed = (int) (1.0 / fSpeed);
        if(1 < iRSpeed)
        {
            /* SLOWMOTION */
            g_playback_info.SlowMotion = iRSpeed;
        }
        else if(1 < iSpeed)
        {
            /* FASTFORWARD */
            g_playback_info.isForwarding = 1;
            g_playback_info.Speed        = iSpeed;
        }
        else if(0 > iSpeed)
        {
            /* FASTBACKWARD */
            g_playback_info.BackWard     = iSpeed;

        }
        else if(1 != iSpeed)
        {
            /* This should never happen */
            //printf("ChangeSpeed - this should never happen fSpeed[%f]\n", fSpeed);
            backend_set_speed(1.0); /* force normal speed */
        }
        InfoStructChanged();
        return 0;
    }

    return -1;
}

static void gstCBsubtitleAvail(GstElement *subsink, GstBuffer *buffer, gpointer user_data)
{
    if (!buffer)
        return;

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
    {
        gst_buffer_unref(buffer);
        return;
    }

    if (GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_DURATION_IS_VALID(buffer))
    {
        gchar *data = g_strndup((const gchar *) map.data, map.size);
        GstMessage *message = gst_message_new_application(GST_OBJECT(g_gst_playbin),
            gst_structure_new ("subtitle",
                "start", GST_TYPE_CLOCK_TIME, GST_BUFFER_PTS(buffer),
                "duration", GST_TYPE_CLOCK_TIME, GST_BUFFER_DURATION(buffer),
                "text", G_TYPE_STRING, data, NULL)
            );
        g_free(data);
        gst_element_post_message(g_gst_playbin, message);
    }
    gst_buffer_unmap(buffer, &map);
    gst_buffer_unref(buffer);
}

static void gstAboutToFinishCallback(GstElement* object, gpointer userdata)
{
    //g_playback_info.isPlaying = 0;

    /* There is no need to wake up sleep
     * 1s delay is acceptable
     */
}

static void gsElementAddedCallback(GstBin *bin, GstElement *element, gpointer data)
{

    gchar *elementname = gst_element_get_name(element);

    {
        if (g_str_has_prefix(elementname, "queue2"))
        {
            if(g_download_buffer_path && strlen(g_download_buffer_path))
            {
                g_object_set(G_OBJECT(element), "temp-template", g_download_buffer_path, NULL);
            }
            else
            {
                g_object_set(G_OBJECT(element), "temp-template", NULL, NULL);
            }
        }
        else if (g_str_has_prefix(elementname, "uridecodebin")
            || g_str_has_prefix(elementname, "decodebin"))
        {
            /*
             * Listen for queue2 element added to uridecodebin/decodebin2 as well.
             * Ignore other bins since they may have unrelated queues
             */
            g_signal_connect(element, "element-added", G_CALLBACK(gsElementAddedCallback), data);
        }
    }
    g_free(elementname);
}

static gboolean isConfigurableHttpSrc(GstElement *element)
{
    /* souphttpsrc: the only source with a cookies property of the format we expect */
    GParamSpec* pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "cookies");
    return (pspec && G_TYPE_STRV == pspec->value_type) ? TRUE : FALSE;
}

/*
 * Apply the -H fields to an http source. Used for playbin's source, the
 * DASH pipeline's source and every http source adaptive demuxers
 * (hlsdemux, dashdemux) create for playlists, fragments and keys.
 */
static void gstApplyHttpHeaders(GstElement *element)
{
    GstStructure *extraHeaders = NULL;
    StrPair_t **headerField = NULL;

    if (!g_ptr_http_header_fields || !element || !isConfigurableHttpSrc(element))
    {
        return;
    }

    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "ssl-strict") != 0)
    {
        g_object_set(G_OBJECT(element), "ssl-strict", FALSE, NULL);
    }

    /* keep what the owner already set (adaptive demuxers add Referer and
     * Cache-Control to fragment sources) and add all our headers to it;
     * setting one structure per header kept only the last one */
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "extra-headers") != 0)
    {
        g_object_get(element, "extra-headers", &extraHeaders, NULL);
    }
    if (!extraHeaders)
    {
        extraHeaders = gst_structure_new_empty("extra-headers");
    }

    for (headerField = g_ptr_http_header_fields; *headerField; ++headerField)
    {
        const gchar *key = (*headerField)->pKey;
        const gchar *val = (*headerField)->pVal;

        if (!strcmp(key, "proxy-id"))
        {
            g_object_set(element, "proxy-id", val, NULL);
        }
        else if (!strcmp(key, "proxy-pw"))
        {
            g_object_set(element, "proxy-pw", val, NULL);
        }
        else if (!strcmp(key, "proxy"))
        {
            g_object_set(element, "proxy", val, NULL);
        }
        else if (!g_ascii_strcasecmp(key, "User-Agent"))
        {
            g_object_set(element, "user-agent", val, NULL);
        }
        else if (!g_ascii_strcasecmp(key, "Cookie"))
        {
            /* one entry: souphttpsrc sends each entry as its own Cookie
             * value, so "a=1; b=2" must not be split (and never at ',') */
            gchar *cookies[] = { (gchar *) val, NULL };
            g_object_set(element, "cookies", cookies, NULL);
        }
        else
        {
            gst_structure_set(extraHeaders, key, G_TYPE_STRING, val, NULL);
        }
    }

    if (gst_structure_n_fields(extraHeaders) > 0)
    {
        g_object_set(element, "extra-headers", extraHeaders, NULL);
    }
    gst_structure_free(extraHeaders);
}

static void gstSourceChangedCallback(GObject *object, GParamSpec *pspec, gpointer data)
{
    GstElement* element = NULL;

    if(!g_ptr_http_header_fields)
    {
        return;
    }

    g_object_get(g_gst_playbin, "source", &element, NULL);
    if (element)
    {
        gstApplyHttpHeaders(element);
        gst_object_unref(element);
    }
}

static void gstSetupIFDSrc(GstElement *element)
{
    if (NULL != g_gstIFDSrc || strcmp(g_type_name(G_OBJECT_TYPE(element)), "GstIFDSrc"))
    {
        return;
    }
    g_gstIFDSrc = GST_ELEMENT_CAST(gst_object_ref(element));
    g_object_set(G_OBJECT(g_gstIFDSrc), "timeout", g_iptv_download_timeout, NULL);
    g_object_set(G_OBJECT(g_gstIFDSrc), "is_live", g_is_live, NULL);
}

static void gstDeepElementAddedCallback(GstBin *bin, GstBin *subBin, GstElement *element, gpointer data)
{
    gstApplyHttpHeaders(element);
    if (g_iptv_download_timeout > 0)
    {
        gstSetupIFDSrc(element);
    }
}

static GstBusSyncReply gstBusSyncHandler(GstBus *bus, GstMessage *message, gpointer user_data)
{
    if(0 < g_sfd)
    {
        int savedErrno = errno; /* In case we change 'errno' */
        write(g_sfd, "x", 1);   /* wake up main loop */
        errno = savedErrno;
    }

    return GST_BUS_PASS;
}

static gboolean gstBusCall(GstBus *bus, GstMessage *msg)
{
    switch (GST_MESSAGE_TYPE(msg))
    {
    case GST_MESSAGE_TAG:
    {
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
    {
        /* We are only interested in state-changed messages from the pipeline */
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(g_gst_playbin))
        {
            GstState old_state, new_state, pending_state;
            gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);

            if(old_state != new_state)
            {
                // g_print ("\nPipeline state changed from %s to %s:\n",
                // gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));
                switch(new_state)
                {
                    case GST_STATE_PAUSED:
                    {
                        g_playback_info.isPaused = 1;
                        backend_query_duration(NULL);
                        break;
                    }
                    case GST_STATE_PLAYING:
                    {
                        backend_query_duration(NULL);
                        g_playback_info.isPaused = 0;
                        break;
                    }
                    case GST_STATE_NULL:
                    case GST_STATE_READY:
                    default:
                        break;
                }


                GstStateChange transition = (GstStateChange)GST_STATE_TRANSITION(old_state, new_state);

                switch(transition)
                {
                    case GST_STATE_CHANGE_NULL_TO_READY:
                    {
                        if (g_iptv_download_timeout > 0 && NULL == g_gstIFDSrc)
                        {
                            GValue result = { 0, };
                            GstIterator *children = gst_bin_iterate_recurse(GST_BIN(g_gst_playbin));
                            if (gst_iterator_find_custom(children, (GCompareFunc)match_sinktype, &result, (gpointer)"GstIFDSrc"))
                            {
                                g_gstIFDSrc = GST_ELEMENT_CAST(g_value_dup_object(&result));
                                g_value_unset(&result);
                                g_object_set(G_OBJECT(g_gstIFDSrc), "timeout", g_iptv_download_timeout, NULL);
                                g_object_set(G_OBJECT(g_gstIFDSrc), "is_live", g_is_live, NULL);
                            }
                            gst_iterator_free(children);
                        }

                        gst_element_set_state(g_gst_playbin, GST_STATE_PAUSED);
                        g_playback_info.isReady = 0;
                    }
                    break;
                    case GST_STATE_CHANGE_READY_TO_PAUSED:
                    {
                        GValue result = { 0, };

                        GstIterator *children = NULL;
                        if (g_dvbAudioSink)
                        {
                            gst_object_unref(GST_OBJECT(g_dvbAudioSink));
                            g_dvbAudioSink = NULL;
                        }
                        if (g_dvbVideoSink)
                        {
                            gst_object_unref(GST_OBJECT(g_dvbVideoSink));
                            g_dvbVideoSink = NULL;
                        }
                        children = gst_bin_iterate_recurse(GST_BIN(g_gst_playbin));
                        if (gst_iterator_find_custom(children, (GCompareFunc)match_sinktype, &result, (gpointer)"GstDVBAudioSink"))
                        {
                            g_dvbAudioSink = GST_ELEMENT_CAST(g_value_dup_object(&result));
                            g_value_unset(&result);
                        }
                        gst_iterator_free(children);
                        children = gst_bin_iterate_recurse(GST_BIN(g_gst_playbin));
                        if (gst_iterator_find_custom(children, (GCompareFunc)match_sinktype, &result, (gpointer)"GstDVBVideoSink"))
                        {
                            g_dvbVideoSink = GST_ELEMENT_CAST(g_value_dup_object(&result));
                            g_value_unset(&result);
                        }
                        gst_iterator_free(children);

                        //m_is_live = (gst_element_get_state(g_gst_playbin, NULL, NULL, 0LL) == GST_STATE_CHANGE_NO_PREROLL);
                    }    break;
                    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
                    {

                    }    break;
                    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
                    {

                    }    break;
                    case GST_STATE_CHANGE_PAUSED_TO_READY:
                    {
                        if (g_dvbAudioSink)
                        {
                            gst_object_unref(GST_OBJECT(g_dvbAudioSink));
                            g_dvbAudioSink = NULL;
                        }
                        if (g_dvbVideoSink)
                        {
                            gst_object_unref(GST_OBJECT(g_dvbVideoSink));
                            g_dvbVideoSink = NULL;
                        }
                    }    break;
                    case GST_STATE_CHANGE_READY_TO_NULL:
                    {
                        g_playback_info.isPlaying = 0;
                    }   break;
                }
                InfoStructChanged();
            }
        }
        break;
    }
    case GST_MESSAGE_EOS:
        {
            g_playback_info.isPlaying = 0;
            InfoStructChanged();
            break;
        }
    case GST_MESSAGE_ERROR:
        {
            gchar *debug = NULL;
            GError *err  = NULL;

            gst_message_parse_error(msg, &err, &debug);
            //fprintf(stderr, "Debug: %s\n", debug);
            g_free(debug);

            gchar *escapedMsg = json_escape(err->message);
            fprintf(stderr, "{\"GST_ERROR\":{\"msg\":\"%s\",\"code\":%i}}\n", escapedMsg, err->code);
            g_free(escapedMsg);
            g_error_free(err);

            g_playback_info.isPlaying = 0;
            InfoStructChanged();
            break;
        }
    case GST_MESSAGE_DURATION:
    {
        break;
    }
    case GST_MESSAGE_ASYNC_DONE:
    {
        g_playback_info.AVSync = 1;
        TracksMessageAsyncDone();
        InfoStructChanged();
        backend_query_duration(NULL);
        backend_query_position(NULL);

        if (0 == g_playback_info.isReady)
        {
            g_playback_info.isReady = 1;
            if (g_playback_info.isPaused)
            {
                gst_element_set_state(g_gst_playbin, GST_STATE_PLAYING);
            }
        }
        break;
    }

    case GST_MESSAGE_ELEMENT:
    {
        if ( gst_is_missing_plugin_message(msg) )
        {
            gchar *description = gst_missing_plugin_message_get_description(msg);
            if( description )
            {
                g_debug("GStreamer plugin [%s] not available!\n", description);
                gchar *escapedDesc = json_escape(description);
                fprintf(stderr, "{\"GST_MISSING_PLUGIN\":{\"msg\":\"%s\"}}\n", escapedDesc);
                g_free(escapedDesc);
                g_free(description);
            }
        }
        else
        {
            const GstStructure *msgstruct = gst_message_get_structure(msg);
            if(NULL != msgstruct)
            {
                const gchar *eventname = gst_structure_get_name(msgstruct);
                if( eventname )
                {
                    if (!strcmp(eventname, "eventSizeChanged") || !strcmp(eventname, "eventSizeAvail"))
                    {
                        int aspect = 0;
                        int width  = 0;
                        int height = 0;
                        gst_structure_get_int (msgstruct, "aspect_ratio", &aspect);
                        gst_structure_get_int (msgstruct, "width", &width);
                        gst_structure_get_int (msgstruct, "height", &height);
                        UpdateVideoTrackInf_1(aspect, width, height);
                        // printf("eventSizeChanged\n");
                    }
                    else if (!strcmp(eventname, "eventFrameRateChanged") || !strcmp(eventname, "eventFrameRateAvail"))
                    {
                        int framerate = 0;
                        gst_structure_get_int (msgstruct, "frame_rate", &framerate);
                        UpdateVideoTrackInf_2((unsigned int)framerate);
                        // printf("eventFrameRateChanged framerate[%d]\n", framerate);

                    }
                    else if (!strcmp(eventname, "eventProgressiveChanged") || !strcmp(eventname, "eventProgressiveAvail"))
                    {
                        int progressive = 0;
                        gst_structure_get_int (msgstruct, "progressive", &progressive);
                        UpdateVideoTrackInf_3((unsigned int)progressive);
                        // printf("eventProgressiveChanged\n");
                    }
                }
            }
        }
        break;
    }
    case GST_MESSAGE_BUFFERING:
    {
        gint percent = 0;
        gst_message_parse_buffering (msg, &percent);
        g_playback_info.BufferingPercent = percent;
        break;
    }
    case GST_MESSAGE_CLOCK_LOST:
    {
        /* Get a new clock */
        gst_element_set_state(g_gst_playbin, GST_STATE_PAUSED);
        gst_element_set_state(g_gst_playbin, GST_STATE_PLAYING);
        break;
    }
    case GST_MESSAGE_APPLICATION:
    {
        const GstStructure *msgstruct = gst_message_get_structure(msg);
        if (NULL != msgstruct)
        {
            const gchar *messagename = gst_structure_get_name(msgstruct);
            if (!strcmp(messagename, "subtitle"))
            {
                const gchar *text;
                GstClockTime start = GST_CLOCK_TIME_NONE;
                GstClockTime duration = GST_CLOCK_TIME_NONE;
                gst_structure_get_clock_time(msgstruct, "start", &start);
                gst_structure_get_clock_time(msgstruct, "duration", &duration);
                text = gst_structure_get_string(msgstruct, "text");
                gchar *escapedText = json_escape(text);
                fprintf(stderr, "{\"PLAYBACK_SUBTITLE\":{\"start\":%lld, \"duration\":%lld, \"text\":\"%s\"}}\n", GST_TIME_AS_MSECONDS(start), GST_TIME_AS_MSECONDS(duration), escapedText);
                g_free(escapedText);
            }
        }
        break;
    }
    default:
        break;
    }

    return TRUE;
}

void backend_gst_poll()
{
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_gst_playbin));
    GstMessage *message = NULL;
    while((message = gst_bus_pop(bus)))
    {
        gstBusCall(bus, message);
        gst_message_unref (message);
    }
    gst_object_unref(bus);
}

const PlaybackInfo_t* backend_get_playback_info()
{
    return &g_playback_info;
}

static gboolean isDashUri(const gchar* uri)
{
    if (!uri) return FALSE;
    gchar *lower = g_ascii_strdown(uri, -1);
    gboolean is_dash = (strstr(lower, ".mpd") != NULL) ||
                       (strstr(lower, "manifest.mpd") != NULL) ||
                       (strstr(lower, "application/dash+xml") != NULL);
    g_free(lower);
    return is_dash;
}

static gchar* gstLaunchQuote(const gchar* value)
{
    GString *s = g_string_new("\"");
    for (const gchar *p = value; *p; ++p) {
        if (*p == '\\' || *p == '"') g_string_append_c(s, '\\');
        g_string_append_c(s, *p);
    }
    g_string_append_c(s, '"');
    return g_string_free(s, FALSE);
}

static void gstSetBoolIfAvailable(GstElement* element, const gchar* property, gboolean value)
{
    if (!element || !property) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), property))
        g_object_set(G_OBJECT(element), property, value, NULL);
}

static void gstSetStringIfAvailable(GstElement* element, const gchar* property, const gchar* value)
{
    if (!element || !property || !value || !*value) return;
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), property))
        g_object_set(G_OBJECT(element), property, value, NULL);
}

/* AML boxes don't ship dvbmediasink; probe + fall back. */
static const gchar* pickFactory(const gchar *caller_choice, const gchar *aml_name, const gchar *dvb_name)
{
    if (caller_choice && *caller_choice) return caller_choice;
    GstElementFactory *f = gst_element_factory_find(aml_name);
    if (f) { gst_object_unref(f); return aml_name; }
    return dvb_name;
}

static GstElement* createDashPlaybackPipeline(const gchar* uri, const gchar *videosink, const gchar *audiosink)
{
    const gchar *vsink_factory = pickFactory(videosink, "dreamvideosink", "dvbvideosink");
    const gchar *asink_factory = pickFactory(audiosink, "dreamaudiosink", "dvbaudiosink");
    gchar *quoted_uri = gstLaunchQuote(uri);
    gchar *pipeline = g_strdup_printf(
        "souphttpsrc name=dashsrc location=%s timeout=60 retries=20 "
        "! dashdemux name=d connection-speed=4000 max-bitrate=3200000 "
        "max-video-width=1280 max-video-height=720 max-video-framerate=50/1 "
        "presentation-delay=6s "
        "d.video_00 ! queue max-size-buffers=0 max-size-bytes=4194304 max-size-time=5000000000 "
        "! qtdemux ! h264parse "
        "! video/x-h264,stream-format=avc,alignment=au "
        "! %s name=dashvideosink "
        "d.audio_00 ! queue max-size-buffers=0 max-size-bytes=1048576 max-size-time=5000000000 "
        "! qtdemux ! aacparse "
        "! audio/mpeg,mpegversion=4,framed=true,stream-format=raw "
        "! %s name=dashaudiosink",
        quoted_uri, vsink_factory, asink_factory);
    g_free(quoted_uri);

    GError *err = NULL;
    GstElement *element = gst_parse_launch(pipeline, &err);
    g_free(pipeline);
    if (!element) {
        g_warning("[gstplayer2] DASH pipeline parse failed: %s", err ? err->message : "unknown");
        if (err) g_error_free(err);
        return NULL;
    }
    if (err) {
        g_warning("[gstplayer2] DASH pipeline parse warning: %s", err->message);
        g_error_free(err);
    }
    /* e2-sync/e2-async no-ops on dream*sinks (property missing). */
    GstElement *vsink = gst_bin_get_by_name(GST_BIN(element), "dashvideosink");
    if (vsink) {
        gstSetBoolIfAvailable(vsink, "e2-sync",  FALSE);
        gstSetBoolIfAvailable(vsink, "e2-async", FALSE);
        gst_object_unref(vsink);
    }
    GstElement *asink = gst_bin_get_by_name(GST_BIN(element), "dashaudiosink");
    if (asink) {
        gstSetBoolIfAvailable(asink, "e2-sync",  FALSE);
        gstSetBoolIfAvailable(asink, "e2-async", FALSE);
        gst_object_unref(asink);
    }
    return element;
}

void backend_init(int *argc, char **argv[], const int sfd)
{
    g_sfd = sfd;
    gst_init(argc, argv);
}

int backend_play(gchar *filename, gchar *download_buffer_path, guint64 ring_buffer_max_size, gint64 buffer_duration, gint buffer_size, StrPair_t **http_header_fields, gchar *videosink, gchar *audiosink, gboolean subtitles_enabled)
{
    backend_stop();
    g_filename               = filename;
    g_download_buffer_path   = download_buffer_path;
    g_ring_buffer_max_size   = ring_buffer_max_size;
    g_buffer_duration        = buffer_duration;
    g_buffer_size            = buffer_size;
    g_ptr_http_header_fields = http_header_fields;

    GstPlayFlags flags = g_playbin_flags;

    /* playbin auto-plug stalls the HW video sink on MPEG-DASH/fMP4
     * (pts_video freezes after preroll). Build an explicit pipeline. */
    if (isDashUri(filename))
    {
        g_gst_playbin = createDashPlaybackPipeline(filename, videosink, audiosink);
        if (!g_gst_playbin) return -1;
        if (g_ptr_http_header_fields)
        {
            GstElement *dashsrc = gst_bin_get_by_name(GST_BIN(g_gst_playbin), "dashsrc");
            if (dashsrc)
            {
                gstApplyHttpHeaders(dashsrc);
                gst_object_unref(dashsrc);
            }
            g_signal_connect(g_gst_playbin, "deep-element-added", G_CALLBACK(gstDeepElementAddedCallback), NULL);
        }
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_gst_playbin));
        gst_bus_set_sync_handler(bus, gstBusSyncHandler, NULL, NULL);
        gst_object_unref(bus);
        gst_element_set_state(g_gst_playbin, GST_STATE_PAUSED);
        g_playback_info.isReady          = 0;
        g_playback_info.isPlaying        = 1;
        g_playback_info.isPaused         = 0;
        g_playback_info.BufferingPercent = -1;
        ChangeSpeed(TRUE, 1.0);
        InfoStructChanged();
        return 0;
    }

    g_gst_playbin = gst_element_factory_make("playbin", "gst-player"); //playbin
    if(g_gst_playbin)
    {
        g_signal_connect(g_gst_playbin, "about-to-finish", G_CALLBACK(gstAboutToFinishCallback), NULL);
        if( strstr(filename, "://") )
        {
            if(g_ptr_http_header_fields)
            {
                g_signal_connect(g_gst_playbin, "notify::source", G_CALLBACK(gstSourceChangedCallback), NULL);
                g_signal_connect(g_gst_playbin, "deep-element-added", G_CALLBACK(gstDeepElementAddedCallback), NULL);
            }


            if(0 < ring_buffer_max_size || 0 <= buffer_duration || 0 <= buffer_size)
            {
                if(g_download_buffer_path && strlen(g_download_buffer_path) && ring_buffer_max_size)
                {
                    /* use progressive download buffering */
                    flags |= GST_PLAY_FLAG_DOWNLOAD;
                    g_signal_connect(G_OBJECT(g_gst_playbin), "element-added", G_CALLBACK(gsElementAddedCallback), NULL);
                    /* limit file size */
                    g_object_set(g_gst_playbin, "ring-buffer-max-size", (guint64)(ring_buffer_max_size * 1024L), NULL);
                }
                /*
                 * regardless whether or not we configured a progressive download file, use a buffer as well
                 * (progressive download might not work for all formats)
                 */
                flags |= GST_PLAY_FLAG_BUFFERING;
                /* increase the default 2 second / 2 MB buffer limitations to 5s / 5MB */
                if(buffer_duration >= 0)
                {
                    g_object_set(G_OBJECT(g_gst_playbin), "buffer-duration", buffer_duration * GST_SECOND, NULL);
                }
                if(g_buffer_size >= 0)
                {
                    /* buffer_size should be set in KB */
                    g_object_set(G_OBJECT(g_gst_playbin), "buffer-size", buffer_size*1024, NULL);
                }
            }
        }
        else
        {
            g_object_set(g_gst_playbin, "buffer-size", (gint)0, NULL);
            g_is_local_file = TRUE;
        }

        {
            GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(g_gst_playbin));
            // gst_bus_add_watch(bus, bus_call, NULL);
            gst_bus_set_sync_handler(bus, gstBusSyncHandler, NULL, NULL);
            gst_object_unref (bus);
        }

        {
            gchar *uri = NULL;

            if(gst_uri_is_valid(filename))
            {
                uri = g_strdup(filename);
            }
            else if(g_path_is_absolute(filename))
            {
                uri = g_filename_to_uri(filename, NULL, NULL);
            }
            else
            {
                gchar *tmp = NULL;
                tmp = g_build_filename(g_get_current_dir(), filename, NULL);
                uri = g_filename_to_uri(tmp, NULL, NULL);
                g_free(tmp);
            }

            g_debug ("%s", uri);
            if(g_iptv_download_timeout > 0)
            {
                g_fileFd = open(filename, O_RDONLY);
            }
            if(g_iptv_download_timeout > 0 && g_fileFd >= 0)
            {
                gchar *fduri = g_strdup_printf ("ifd://%d", g_fileFd);
                g_signal_connect(g_gst_playbin, "deep-element-added", G_CALLBACK(gstDeepElementAddedCallback), NULL);
                g_object_set(G_OBJECT (g_gst_playbin), "uri", fduri, NULL);
                g_free(fduri);
            }
            else
            {
                g_object_set(G_OBJECT (g_gst_playbin), "uri", uri, NULL);
            }

            if (subtitles_enabled)
            {
                g_subsink = gst_element_factory_make("subsink", NULL);
                if (g_subsink)
                {
                    flags |= GST_PLAY_FLAG_TEXT;
                    g_signal_connect (g_subsink, "new-buffer", G_CALLBACK (gstCBsubtitleAvail), NULL);
                    GstCaps *subcaps = gst_caps_from_string("text/plain; text/x-plain; text/x-raw; text/x-pango-markup");
                    g_object_set (G_OBJECT (g_subsink), "caps", subcaps, NULL);
                    gst_caps_unref(subcaps);
                    g_object_set (G_OBJECT (g_gst_playbin), "text-sink", g_subsink, NULL);
                    g_object_set (G_OBJECT (g_gst_playbin), "current-text", -1, NULL);
                }
                else
                {
                    printf("sorry, can't play subtitles: missing gst-plugin-subsink\n");
                }
            }
            g_object_set(G_OBJECT (g_gst_playbin), "flags", flags, NULL);

            if (videosink != NULL)
            {
                GstElement *vsink = gst_element_factory_make(videosink, NULL);
                g_object_set(G_OBJECT (g_gst_playbin), "video-sink", vsink, NULL);
            }
            if (audiosink != NULL)
            {
                GstElement *asink = gst_element_factory_make(audiosink, NULL);
                g_object_set(G_OBJECT (g_gst_playbin), "audio-sink", asink, NULL);
            }
            g_free(uri);
        }

        GstStateChangeReturn sts = gst_element_set_state(g_gst_playbin, GST_STATE_PAUSED);
        //if(sts)
        {
            g_playback_info.isReady = 0;
            g_playback_info.isPlaying = 1;
            g_playback_info.isPaused = 0;
            g_playback_info.BufferingPercent = -1;
            ChangeSpeed(TRUE, 1.0);
            InfoStructChanged();
            return 0;
        }
    }
    return -1;
}

int backend_stop()
{
    int ret = 0;
    if (g_dvbAudioSink)
    {
        gst_object_unref(GST_OBJECT(g_dvbAudioSink));
        g_dvbAudioSink = NULL;
    }
    if (g_dvbVideoSink)
    {
        gst_object_unref(GST_OBJECT(g_dvbVideoSink));
        g_dvbVideoSink = NULL;
    }
    if (g_gstIFDSrc)
    {
        //if (g_iptv_download_timeout > 0 )
        {
            g_iptv_download_timeout = 0;
            g_object_set(G_OBJECT(g_gstIFDSrc), "timeout", g_iptv_download_timeout, NULL);
        }
        gst_object_unref(GST_OBJECT(g_gstIFDSrc));
        g_gstIFDSrc = NULL;
    }
    if(g_gst_playbin)
    {
        GstState st = GST_STATE_NULL;
        GstStateChangeReturn res = gst_element_set_state(g_gst_playbin, GST_STATE_NULL);
        res = gst_element_get_state(g_gst_playbin, &st, 0, 10 * GST_SECOND);
        /* We give 5s for close */
        if(GST_STATE_CHANGE_SUCCESS == res
           && GST_STATE_NULL == st)
        {
            gst_object_unref(GST_OBJECT(g_gst_playbin));
            g_gst_playbin = NULL;
        }
        else
        {
            ret = -1;
        }

    }

    if (g_fileFd >= 0)
    {
        close(g_fileFd);
        g_fileFd = -1;
    }

    memset(&g_playback_info, 0, sizeof(g_playback_info));
    InfoStructChanged();
    return ret;
}

int backend_pause()
{
    int sts = 0;
    if (g_playback_info.isReady)
    {
        if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(g_gst_playbin, GST_STATE_PAUSED))
        {
            sts = -1;
        }
    }
    if (sts == 0)
    {
        g_playback_info.isPaused = 1;
    }
    return sts;
}

int backend_resume()
{
    int sts = 0;
    if (g_playback_info.isReady)
    {
        if(g_playback_info.isForwarding)
        {
            backend_set_speed(1.0);
        }

        if (GST_STATE_CHANGE_FAILURE == gst_element_set_state(g_gst_playbin, GST_STATE_PLAYING))
        {
            sts = -1;
        }
    }

    if (sts == 0)
    {
        g_playback_info.isPaused = 0;
    }
    return sts;
}

int backend_reset()
{
    gboolean result = gst_element_seek( g_gst_playbin, 1.0,
                                        GST_FORMAT_TIME,
                                        g_seek_flags,
                                        GST_SEEK_TYPE_SET, 0,
                                        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );
    return ChangeSpeed(result, 1.0);
}

int backend_seek(const double seconds)
{
    /*
    gint64 time_nanoseconds = (gint64)(seconds * GST_SECOND);
    gboolean result = gst_element_seek( g_gst_playbin, 1.0,
                                        GST_FORMAT_TIME,
                                        g_seek_flags,
                                        GST_SEEK_TYPE_CUR, time_nanoseconds,
                                        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );
    return ChangeSpeed(result, 1.0);
    */
    return -1;
}

int backend_seek_absolute(const double seconds)
{
    gint64 time_nanoseconds = (gint64)(seconds * GST_SECOND);
    gboolean result = gst_element_seek( g_gst_playbin, 1.0,
                                        GST_FORMAT_TIME,
                                        g_seek_flags,
                                        GST_SEEK_TYPE_SET, time_nanoseconds,
                                        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );


    memset(&g_eos_fix, 0, sizeof(g_eos_fix));
    return ChangeSpeed(result, 1.0);
}

int backend_set_speed(const float speed)
{
    GstSeekFlags seek_flags = 1.0 == speed ? g_seek_flags : GST_SEEK_FLAG_NONE;

    gboolean result = gst_element_seek( g_gst_playbin, speed,
                                        GST_FORMAT_TIME,
                                        seek_flags,
                                        GST_SEEK_TYPE_NONE, 0,
                                        GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE );
    return ChangeSpeed(result, speed);
}

int backend_query_position(int64_t *mseconds)
{
    if(mseconds)
    {
        *mseconds = 0;
    }

    if (!g_gst_playbin || !g_playback_info.isPlaying)
    {
        return -1;
    }

    gboolean result = FALSE;
    GstState st;
    GstStateChangeReturn res = gst_element_get_state(g_gst_playbin, &st, 0, 0);
    //fprintf(stderr, "{gst_element_get_state res[%d] st[%d] GST_STATE_CHANGE_SUCCESS[%d], GST_STATE_PLAYING[%d]\n}", (int)res, (int)st, GST_STATE_CHANGE_SUCCESS, GST_STATE_PLAYING) ;
    if(GST_STATE_CHANGE_SUCCESS == res
       && GST_STATE_PLAYING == st)
    {
        GstFormat format    = GST_FORMAT_TIME;
        gint64 playbin_time = GST_CLOCK_TIME_NONE;
        gint64 decoder_time = GST_CLOCK_TIME_NONE;

        result = gst_element_query_position(g_gst_playbin, format, &playbin_time);
        if(!result)
        {
            playbin_time = 0;
        }
        /*
        else
        {
            test(playbin_time);
        }
        */
        if ( g_dvbAudioSink || g_dvbVideoSink)
        {
            g_signal_emit_by_name(g_dvbVideoSink?g_dvbVideoSink:g_dvbAudioSink, "get-decoder-time", &decoder_time);
            /* EOS fix start */
            gint64 timestamp = getTimestamp();
            if(0 == g_eos_fix.check_timestamp) g_eos_fix.check_timestamp = timestamp;

            //fprintf(stderr, "{d:%lld p:%lld}\n", decoder_time, playbin_time);
            if(decoder_time  == g_eos_fix.decoder_time &&
               playbin_time  == g_eos_fix.playbin_time )
            {
                if(10000 < (timestamp-g_eos_fix.check_timestamp) )
                {
                    backend_stop();
                }
            }
            else
            {
                g_eos_fix.check_timestamp = timestamp;
                g_eos_fix.decoder_time    = decoder_time;
                g_eos_fix.playbin_time    = playbin_time;
            }
            /* EOS fix end */
        }
        else
        {
            decoder_time = playbin_time;
        }

        //fprintf(stderr, "{p:%lld result[%d]}\n", playbin_time, result);
        if (result && GST_FORMAT_TIME == format)
        {
            result = FALSE;
            if(GST_CLOCK_TIME_IS_VALID(decoder_time))
            {
                /* validate time from decoder */
                gint64 diff = (decoder_time > playbin_time) ? decoder_time - playbin_time : playbin_time - decoder_time;
                if( GST_TIME_AS_SECONDS(diff) < 180)
                {
                    result = TRUE;
                    if(mseconds)
                    {
                        *mseconds = (int)GST_TIME_AS_MSECONDS(decoder_time);
                    }
                    else
                    {
                        fprintf(stderr, "{\"J\":{\"ms\":%lld}}\n", (int64_t)GST_TIME_AS_MSECONDS(decoder_time));
                    }
                }
            }
        }
    }
    return result ? 0 : -1;
}

int backend_query_duration(double *length)
{
    double tmpLength = 0;
    GstFormat format = GST_FORMAT_TIME;
    gint64 duration = GST_CLOCK_TIME_NONE;
    gboolean result;

    result = gst_element_query_duration (g_gst_playbin, format, &duration);
    if (!result || format != GST_FORMAT_TIME)
    {
        if(length)
        {
            *length = 0;
        }
        return -1;
    }
    g_duration = duration;
    tmpLength = duration / ((double) GST_SECOND);
    if(!length)
    {
        fprintf(stderr, "{\"PLAYBACK_LENGTH\":{\"length\":%lf, \"sts\":0}}\n", tmpLength);
        /* To do: fill audio list, video list .etc */
    }
    else
    {
        *length = tmpLength;
    }
    return 0;
}

void backend_deinit()
{
    backend_stop();
    g_sfd = -1;
}

int backend_set_download_timeout(const uint64_t mseconds)
{
    g_iptv_download_timeout = mseconds;
    if (NULL != g_gstIFDSrc)
    {
        g_object_set(G_OBJECT(g_gstIFDSrc), "timeout", g_iptv_download_timeout, NULL);
    }
    return 0;
}

int backend_set_is_live(const uint8_t live)
{
    g_is_live = live > 0 ? TRUE : FALSE;
    if (NULL != g_gstIFDSrc)
    {
        g_object_set(G_OBJECT(g_gstIFDSrc), "is_live", g_is_live, NULL);
    }
    return 0;
}
