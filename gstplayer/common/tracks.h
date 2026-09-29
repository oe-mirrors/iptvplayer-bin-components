#ifndef GST_TRACKS_COMMON_FUNCTIONS
#define GST_TRACKS_COMMON_FUNCTIONS

#include "gst-backend.h"


/* Track */
TrackDescription_t *g_audio_tracks = NULL;
TrackDescription_t *g_video_tracks = NULL;
TrackDescription_t *g_subtitle_tracks = NULL;
int g_audio_num = 0;
int g_video_num = 0;
int g_subtitle_num = 0;
int g_audio_idx = -1;
int g_video_idx = -1;
int g_subtitle_idx = -1;


/* free a track list; the lists are rebuilt each time the tracks are read */
static void TracksFree(TrackDescription_t **tracks, int *num)
{
    int i;
    for (i = 0; NULL != *tracks && i < *num; i++)
    {
        g_free((*tracks)[i].Name);
        g_free((*tracks)[i].Encoding);
    }
    g_free(*tracks);
    *tracks = NULL;
    *num = 0;
}

/* track lines last sent to the caller: after playback start the tracks are
 * read again whenever something may have changed, and only what really
 * changed is reported */
enum
{
    TRACK_LINE_A_L = 0,
    TRACK_LINE_A_C,
    TRACK_LINE_S_L,
    TRACK_LINE_S_C,
    TRACK_LINE_V_C,
    TRACK_LINE_NUM
};
static gchar *g_track_lines[TRACK_LINE_NUM];

/* set (from any thread) when a stream, its caps or its tags changed */
static gint g_tracks_dirty = 0;

/* reading the tracks again after the start, for caps that come late
 * (growing ifd:// files, streams without an early ASYNC_DONE) */
static gint64 g_tracks_retry_at = 0;
static gint64 g_tracks_retry_step = 0;

/* what the video sink reported (eventSizeChanged, ...): caps read again later
 * must not undo it */
static struct
{
    int width;
    int height;
    unsigned int frame_rate;
    int progressive;
} g_video_sink_inf = { 0, 0, 0, -1 };

static TrackDescription_t* ReportCurrentTrack(const char type, gboolean force);

static TrackDescription_t* GetVideoTrackForUpdate()
{
    /* the list skips streams without caps, g_video_idx is the playbin index */
    int i;
    for (i = 0; g_video_tracks && i < g_video_num; ++i)
    {
        if (g_video_tracks[i].Id == g_video_idx)
        {
            return &g_video_tracks[i];
        }
    }
    return NULL;
}

void UpdateVideoTrackInf_1(const int aspect, const int width, const int height)
{
    TrackDescription_t *pVidTrack = GetVideoTrackForUpdate();
    g_video_sink_inf.width = width;
    g_video_sink_inf.height = height;
    if(pVidTrack)
    {
        pVidTrack->width = width;
        pVidTrack->height = height;
        ReportCurrentTrack('v', FALSE);
    }
}

void UpdateVideoTrackInf_2(const unsigned int framerate)
{
    TrackDescription_t *pVidTrack = GetVideoTrackForUpdate();
    g_video_sink_inf.frame_rate = framerate;
    if(pVidTrack)
    {
        pVidTrack->frame_rate = framerate;
        ReportCurrentTrack('v', FALSE);
    }
}

void UpdateVideoTrackInf_3(const unsigned int progressive)
{
    TrackDescription_t *pVidTrack = GetVideoTrackForUpdate();
    g_video_sink_inf.progressive = (int)progressive;
    if(pVidTrack)
    {
        pVidTrack->progressive = (int)progressive;
        ReportCurrentTrack('v', FALSE);
    }
}

static void TracksMarkDirty(void)
{
    /* one wake-up per batch of changes; the main loop reads the tracks */
    if (g_atomic_int_compare_and_exchange(&g_tracks_dirty, 0, 1) && g_gst_playbin)
    {
        gst_element_post_message(g_gst_playbin,
            gst_message_new_application(GST_OBJECT(g_gst_playbin), gst_structure_new_empty("e2i-tracks")));
    }
}

static void TracksCapsNotify(GObject *pad, GParamSpec *pspec, gpointer data)
{
    TracksMarkDirty();
}

/* read the tracks again whenever a stream gets caps: late ones (growing
 * file) and new ones (resolution switch). notify::caps comes after the pad
 * stored them, so the refresh it triggers finds them. Main thread only. */
static void TracksWatchCaps(GstPad *pad)
{
    if (pad && !g_object_get_data(G_OBJECT(pad), "e2i-caps-watch"))
    {
        g_object_set_data(G_OBJECT(pad), "e2i-caps-watch", GINT_TO_POINTER(1));
        g_signal_connect(pad, "notify::caps", G_CALLBACK(TracksCapsNotify), NULL);
    }
}

/* fps * 1000 as the dvb sinks report it (30000/1001 -> 29970) */
static guint TracksFrameRate(const GstStructure *str)
{
    gint num = 0;
    gint denom = 0;
    if (gst_structure_get_fraction(str, "framerate", &num, &denom) && num > 0 && denom > 0)
    {
        return (guint)((num * 1000LL + denom / 2) / denom);
    }
    return 0;
}

static void TracksStreamsChangedCallback(GstElement *playbin, gpointer data)
{
    TracksMarkDirty();
}

static void TracksTagsChangedCallback(GstElement *playbin, gint stream, gpointer data)
{
    TracksMarkDirty();
}

static void TracksConnectPlaybin(GstElement *playbin)
{
    g_signal_connect(playbin, "audio-changed", G_CALLBACK(TracksStreamsChangedCallback), NULL);
    g_signal_connect(playbin, "video-changed", G_CALLBACK(TracksStreamsChangedCallback), NULL);
    g_signal_connect(playbin, "text-changed", G_CALLBACK(TracksStreamsChangedCallback), NULL);
    g_signal_connect(playbin, "audio-tags-changed", G_CALLBACK(TracksTagsChangedCallback), NULL);
    g_signal_connect(playbin, "video-tags-changed", G_CALLBACK(TracksTagsChangedCallback), NULL);
    g_signal_connect(playbin, "text-tags-changed", G_CALLBACK(TracksTagsChangedCallback), NULL);
}

static void TracksReset(void)
{
    int i;
    for (i = 0; i < TRACK_LINE_NUM; ++i)
    {
        g_free(g_track_lines[i]);
        g_track_lines[i] = NULL;
    }
    g_atomic_int_set(&g_tracks_dirty, 0);
    g_tracks_retry_at = 0;
    g_tracks_retry_step = 0;
    g_video_sink_inf.width = 0;
    g_video_sink_inf.height = 0;
    g_video_sink_inf.frame_rate = 0;
    g_video_sink_inf.progressive = -1;
}

static int GetCurrentTrack(const char *type, int *idx)
{
    if (!isPlaybin())
    {
        /* DASH pipeline: one video track, the linked audio adaptation set */
        g_mutex_lock(&g_dash_lock);
        if (!strcmp(type, "current-audio"))
        {
            *idx = g_dash.audioLinked;
        }
        else if (!strcmp(type, "current-video"))
        {
            *idx = g_dash.videoLinked ? 0 : -1;
        }
        else
        {
            *idx = -1;
        }
        g_mutex_unlock(&g_dash_lock);
        return *idx;
    }
    g_object_get(G_OBJECT (g_gst_playbin), type, idx, NULL);
    return *idx;
}

static int SelectSubtitleStream(int i)
{
    int current_subtitle;
    g_object_set (G_OBJECT (g_gst_playbin), "current-text", i, NULL);
    g_object_get (G_OBJECT (g_gst_playbin), "current-text", &current_subtitle, NULL);
    g_subtitle_idx = current_subtitle;
    if ( current_subtitle == i )
    {
        return 0;
    }
    return -1;
}

static int SelectSubtitleTrack(unsigned int i)
{
    int ret = -1;
    if (!isPlaybin())
    {
        /* DASH pipeline: subtitle adaptation sets are not played */
        return -1;
    }
    if (g_subtitle_num > 0)
    {
        int validposition = 0;
        int64_t ppos = 0;

        if (0 == backend_query_position(&ppos))
        {
            validposition = 1;
            ppos -= 1000;
            if (ppos < 0)
            {
                ppos = 0;
            }
        }
        if (validposition)
        {
            /* flush */
            double dpos = ppos/1000.0;
            backend_seek_absolute(dpos);
        }
        ret = SelectSubtitleStream(i);
    }
    else
    {
        g_subtitle_idx = i;
        ret = 0;
    }
    return ret;
}


static int SelectAudioStream(int i)
{
    int current_audio;
    g_object_set (G_OBJECT (g_gst_playbin), "current-audio", i, NULL);
    g_object_get (G_OBJECT (g_gst_playbin), "current-audio", &current_audio, NULL);
    g_audio_idx = current_audio;
    if ( current_audio == i )
    {
        return 0;
    }
    return -1;
}

static int SelectAudioTrack(unsigned int i)
{
    int ret = -1;
    if (!isPlaybin())
    {
        /* DASH pipeline: the audio set is chosen when the demuxer exposes
         * its pads (-i); switching while playing is not supported yet */
        if (g_audio_num == 0)
        {
            g_audio_idx = i;
            return 0;
        }
        g_audio_idx = g_dash.audioLinked;
        return ((int)i == g_dash.audioLinked) ? 0 : -1;
    }
    if (g_audio_num > 0)
    {
        int validposition = 0;
        int64_t ppos = 0;

        if (0 == backend_query_position(&ppos))
        {
            validposition = 1;
            ppos -= 1000;
            if (ppos < 0)
            {
                ppos = 0;
            }
        }
        if (validposition)
        {
            /* flush */
            double dpos = ppos/1000.0;
            backend_seek_absolute(dpos);
        }
        ret = SelectAudioStream(i);
    }
    else
    {
        g_audio_idx = i;
        ret = 0;
    }
    return ret;
}

static int SetStr(char **set, const char *val)
{
    int ret = -1;
    if (NULL != set && NULL != val)
    {
        g_free(*set);
        *set = g_strdup(val);
        ret = 0;
    }
    return ret;
}

static void FillAudioTracks()
{
    gint i = 0;
    gint n_audio = 0;

    TracksFree(&g_audio_tracks, &g_audio_num);

    if (!isPlaybin())
    {
        /* DASH pipeline: one track per audio adaptation set */
        g_mutex_lock(&g_dash_lock);
        n_audio = g_dash.audioPads ? (gint)g_dash.audioPads->len : 0;
        g_audio_tracks = g_new0(TrackDescription_t, n_audio);
        for (i = 0; i < n_audio; i++)
        {
            GstPad *pad = g_ptr_array_index(g_dash.audioPads, i);
            GstEvent *tagEvent = NULL;
            gchar *lang = g_strdup(g_object_get_data(G_OBJECT(pad), "e2i-lang"));
            guint idx = 0;
            TrackDescription_t *track = &g_audio_tracks[i];
            /* a pad can hold several tag events (global and stream scope,
             * later ones from downstream elements) */
            while (!lang && NULL != (tagEvent = gst_pad_get_sticky_event(pad, GST_EVENT_TAG, idx++)))
            {
                GstTagList *tags = NULL;
                gst_event_parse_tag(tagEvent, &tags);
                if (tags)
                {
                    gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &lang);
                }
                gst_event_unref(tagEvent);
            }
            track->Id = i;
            SetStr(&(track->Name), lang ? lang : "und");
            SetStr(&(track->Encoding), (i == g_dash.audioLinked && g_dash.audioEncoding) ? g_dash.audioEncoding : "audio/mp4");
            g_free(lang);
        }
        g_mutex_unlock(&g_dash_lock);
        g_audio_num = n_audio;
        return;
    }

    g_object_get(g_gst_playbin, "n-audio", &n_audio, NULL);

    g_audio_tracks = g_new0(TrackDescription_t, n_audio);

    int j = 0;
    for (i = 0; i < n_audio; i++)
    {
        gchar *g_codec   = NULL;
        gchar *g_lang    = NULL;
        GstTagList *tags = NULL;
        GstPad* pad = 0;
        g_signal_emit_by_name (g_gst_playbin, "get-audio-pad", i, &pad);
        if (!pad)
        {
            continue;
        }
#if GST_VERSION_MAJOR < 1
        GstCaps* caps = gst_pad_get_negotiated_caps(pad);
#else
        GstCaps* caps = gst_pad_get_current_caps(pad);
#endif
        TracksWatchCaps(pad);
        gst_object_unref(pad);
        if (!caps)
        {
            continue;
        }
        GstStructure* str = gst_caps_get_structure(caps, 0);
        const gchar *g_type = gst_structure_get_name(str);

        TrackDescription_t *track = &g_audio_tracks[j];
        track->Id = i;
        SetStr(&(track->Name), "und");
        SetStr(&(track->Encoding), (char *)g_type);

        g_codec = NULL;
        g_lang = NULL;
        g_signal_emit_by_name (g_gst_playbin, "get-audio-tags", i, &tags);
#if GST_VERSION_MAJOR < 1
        if (tags && gst_is_tag_list(tags))
#else
        if (tags && GST_IS_TAG_LIST(tags))
#endif
        {
            if (gst_tag_list_get_string(tags, GST_TAG_AUDIO_CODEC, &g_codec))
            {
                SetStr(&(track->Encoding), (char *)g_codec);
                g_free(g_codec);
            }
            if (gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &g_lang))
            {
                SetStr(&(track->Name), (char *)g_lang);
                g_free(g_lang);
            }
            gst_tag_list_free(tags);
        }
        gst_caps_unref(caps);
        ++j;
    }
    g_audio_num = j;
}

static void FillVideoTracks()
{
    GstPad *videopad = NULL;
    int n_video = 0;
    int i = 0;
    int j = 0;

    /* read again every time: the caps may not have been there before */
    TracksFree(&g_video_tracks, &g_video_num);

    if (!isPlaybin())
    {
        /* DASH pipeline: the one linked video stream */
        g_mutex_lock(&g_dash_lock);
        if (g_dash.vsink && g_dash.videoEncoding)
        {
            g_video_tracks = g_new0(TrackDescription_t, 1);
            g_video_tracks[0].Id = 0;
            SetStr(&(g_video_tracks[0].Name), "und");
            SetStr(&(g_video_tracks[0].Encoding), g_dash.videoEncoding);
            g_video_tracks[0].width = g_dash.videoWidth;
            g_video_tracks[0].height = g_dash.videoHeight;
            g_video_tracks[0].frame_rate = g_dash.videoFrameRate;
            g_video_tracks[0].progressive = -1;
            g_video_num = 1;
        }
        g_mutex_unlock(&g_dash_lock);
    }
    else
    {
        g_object_get(g_gst_playbin, "n-video", &n_video, NULL);
    }

    if (n_video > 0)
    {
        g_video_tracks = g_new0(TrackDescription_t, n_video);

        for (i = 0; i < n_video; i++)
        {
            videopad = NULL;
            g_signal_emit_by_name(g_gst_playbin, "get-video-pad", i, &videopad);
            if (videopad)
            {
#if GST_VERSION_MAJOR < 1
                GstCaps* caps = gst_pad_get_negotiated_caps(videopad);
#else
                GstCaps* caps = gst_pad_get_current_caps(videopad);
#endif
                TracksWatchCaps(videopad);
                if (caps)
                {
                    GstStructure *str = gst_caps_get_structure (caps, 0);
                    if(str)
                    {
                        const gchar *g_type = gst_structure_get_name(str);

                        TrackDescription_t *track = &g_video_tracks[j];
                        track->Id = i;
                        SetStr(&(track->Name), "und");
                        SetStr(&(track->Encoding), (char *)g_type);

                        gst_structure_get_int(str, "width",  &track->width);
                        gst_structure_get_int(str, "height", &track->height);
                        track->frame_rate = TracksFrameRate(str);
                        track->progressive = -1;
                        ++j;
                    }
                    gst_caps_unref(caps);
                }
                gst_object_unref (videopad);
            }
        }
        g_video_num = j;
    }

    /* what the sink reported wins: it shows what is really decoded */
    if (g_video_num > 0)
    {
        TrackDescription_t *track = NULL;
        GetCurrentTrack("current-video", &g_video_idx);
        track = GetVideoTrackForUpdate();
        if (track)
        {
            if (g_video_sink_inf.width > 0 && g_video_sink_inf.height > 0)
            {
                track->width = g_video_sink_inf.width;
                track->height = g_video_sink_inf.height;
            }
            if (g_video_sink_inf.frame_rate > 0)
            {
                track->frame_rate = g_video_sink_inf.frame_rate;
            }
            if (g_video_sink_inf.progressive >= 0)
            {
                track->progressive = g_video_sink_inf.progressive;
            }
        }
    }
}

static void FillSubtitlesTracks()
{
    gint i = 0;
    gint n_subtitles = 0;

    TracksFree(&g_subtitle_tracks, &g_subtitle_num);

    if (!isPlaybin())
    {
        /* DASH pipeline: subtitle adaptation sets are not played */
        g_subtitle_num = 0;
        return;
    }

    g_object_get(g_gst_playbin, "n-text", &n_subtitles, NULL);

    g_subtitle_tracks = g_new0(TrackDescription_t, n_subtitles);

    int j = 0;
    for (i = 0; i < n_subtitles; i++)
    {
        gchar *g_codec   = NULL;
        gchar *g_lang    = NULL;
        GstTagList *tags = NULL;
        GstPad* pad = 0;
        g_signal_emit_by_name (g_gst_playbin, "get-text-pad", i, &pad);
        if (!pad)
        {
            continue;
        }
        GstCaps* caps = gst_pad_get_current_caps(pad);
        TracksWatchCaps(pad);
        gst_object_unref(pad);
        if (!caps)
        {
            continue;
        }
        GstStructure* str = gst_caps_get_structure(caps, 0);
        const gchar *g_type = gst_structure_get_name(str);

        TrackDescription_t *track = &g_subtitle_tracks[j];
        track->Id = i;
        SetStr(&(track->Name), "und");
        SetStr(&(track->Encoding), (char *)g_type);

        g_signal_emit_by_name (g_gst_playbin, "get-text-tags", i, &tags);
        if (tags && GST_IS_TAG_LIST(tags))
        {
            if (gst_tag_list_get_string(tags, GST_TAG_SUBTITLE_CODEC, &g_codec))
            {
                SetStr(&(track->Encoding), (char *)g_codec);
                g_free(g_codec);
            }
            if (gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &g_lang))
            {
                SetStr(&(track->Name), (char *)g_lang);
                g_free(g_lang);
            }
            gst_tag_list_free(tags);
        }
        gst_caps_unref(caps);
        ++j;
    }
    g_subtitle_num = j;
}

static gchar* TracksListLine(const char type, const TrackDescription_t *pTracks, const int num)
{
    GString *out = g_string_new(NULL);
    int i = 0;
    g_string_append_printf(out, "{\"%c_l\": [", type);
    for (i = 0; i < num; ++i)
    {
        gchar *e = json_escape(pTracks[i].Encoding);
        gchar *n = json_escape(pTracks[i].Name);
        g_string_append_printf(out, "%s{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\"}", (0 < i) ? ", " : "", pTracks[i].Id, e, n);
        g_free(e);
        g_free(n);
    }
    g_string_append(out, "]}");
    return g_string_free(out, FALSE);
}

/* send a track line; unless asked for it, only when it changed */
static void TrackLineSend(const int slot, gchar *line, const gboolean force)
{
    if (force || g_strcmp0(line, g_track_lines[slot]))
    {
        fprintf(stderr, "%s\n", line);
        g_free(g_track_lines[slot]);
        g_track_lines[slot] = line;
    }
    else
    {
        g_free(line);
    }
}

/*
 * Read all tracks again and report what changed. Called on ASYNC_DONE, when
 * playbin/dashdemux announce new streams, caps or tags, and a few times
 * after the start - a stream is often not complete at the first ASYNC_DONE
 * (growing ifd:// file, sinks that preroll without data), and before this
 * the tracks were only read then and never again.
 * A track chosen before the list was known (-i, or "a<N>" too early) is
 * selected once the list is there.
 */
static void TracksRefresh(void)
{
    int wanted = -1;

    wanted = (0 == g_audio_num) ? g_audio_idx : -1;
    FillAudioTracks();
    /* an empty list is no news (pads without caps for a moment, after EOS) */
    if (g_audio_num > 0)
    {
        TrackLineSend(TRACK_LINE_A_L, TracksListLine('a', g_audio_tracks, g_audio_num), FALSE);
        GetCurrentTrack("current-audio", &g_audio_idx);
        if (wanted >= 0 && wanted != g_audio_idx)
        {
            backend_set_track('a', wanted);
        }
        ReportCurrentTrack('a', FALSE);
    }

    wanted = (0 == g_subtitle_num) ? g_subtitle_idx : -1;
    FillSubtitlesTracks();
    if (g_subtitle_num > 0)
    {
        TrackLineSend(TRACK_LINE_S_L, TracksListLine('s', g_subtitle_tracks, g_subtitle_num), FALSE);
        GetCurrentTrack("current-text", &g_subtitle_idx);
        if (wanted >= 0 && wanted != g_subtitle_idx)
        {
            backend_set_track('s', wanted);
        }
        ReportCurrentTrack('s', FALSE);
    }

    FillVideoTracks();
    if (g_video_num > 0)
    {
        ReportCurrentTrack('v', FALSE);
    }
}

/* the stream runs: look at the tracks again 0.5, 1.5, 3.5, ... 31.5 s later */
static void TracksStartRetries(void)
{
    g_tracks_retry_step = 500;
    g_tracks_retry_at = g_get_monotonic_time() / 1000 + g_tracks_retry_step;
}

/* main loop, after the bus messages */
static void TracksPoll(void)
{
    gboolean due = g_atomic_int_compare_and_exchange(&g_tracks_dirty, 1, 0);
    if (g_tracks_retry_at > 0)
    {
        gint64 now = g_get_monotonic_time() / 1000;
        if (now >= g_tracks_retry_at)
        {
            due = TRUE;
            g_tracks_retry_step *= 2;
            g_tracks_retry_at = (g_tracks_retry_step <= 16000) ? now + g_tracks_retry_step : 0;
        }
    }
    if (due && g_gst_playbin && g_playback_info.isPlaying)
    {
        TracksRefresh();
    }
}

TrackDescription_t* backend_get_tracks_list(const char type, int *num)
{
    int localNum = 0;
    int slot = TRACK_LINE_A_L;
    TrackDescription_t *pTracks = NULL;
    if ('a' == type)
    {
        FillAudioTracks();
        pTracks = g_audio_tracks;
        localNum = g_audio_num;
    }
    else if ('s' == type)
    {
        FillSubtitlesTracks();
        pTracks = g_subtitle_tracks;
        localNum = g_subtitle_num;
        slot = TRACK_LINE_S_L;
    }

    if (NULL != num)
    {
        *num = localNum;
    }

    if ('a' == type || 's' == type)
    {
        TrackLineSend(slot, TracksListLine(type, pTracks, localNum), TRUE);
    }

    return pTracks;
}

static TrackDescription_t* ReportCurrentTrack(const char type, gboolean force)
{
    int idx = -1;
    int num = 0;
    int slot = TRACK_LINE_V_C;
    TrackDescription_t *pTracks = NULL;

    TrackDescription_t *track = NULL;
    if ('a' == type)
    {
        pTracks = g_audio_tracks;
        idx     = GetCurrentTrack("current-audio", &g_audio_idx);
        num     = g_audio_num;
        slot    = TRACK_LINE_A_C;
    }
    else if ('s' == type)
    {
        pTracks = g_subtitle_tracks;
        idx     = GetCurrentTrack("current-text", &g_subtitle_idx);
        num     = g_subtitle_num;
        slot    = TRACK_LINE_S_C;
    }
    else if ('v' == type)
    {
        pTracks = g_video_tracks;
        idx     = GetCurrentTrack("current-video", &g_video_idx);
        num     = g_video_num;
    }

    if (idx >= 0 && NULL != pTracks)
    {
        /* the lists skip streams without caps, so the playbin index is not
         * always the position in the list */
        int i;
        for (i = 0; i < num; ++i)
        {
            if (pTracks[i].Id == idx)
            {
                track = &pTracks[i];
                break;
            }
        }
    }

    if (NULL != track)
    {
        gchar *e = json_escape(track->Encoding);
        gchar *n = json_escape(track->Name);
        gchar *line = NULL;
        if ('a' == type || 's' == type)
        {
            line = g_strdup_printf("{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\"}}", type, 'c', track->Id , e, n);
        }
        else // video
        {
            // information about only current video track will be stored
            line = g_strdup_printf("{\"%c_%c\":{\"id\":%d,\"e\":\"%s\",\"n\":\"%s\",\"w\":%d,\"h\":%d,\"f\":%u,\"p\":%d}}", type, 'c', track->Id , e, n, track->width, track->height, track->frame_rate, track->progressive);
        }
        TrackLineSend(slot, line, force);
        g_free(e);
        g_free(n);
    }
    return track;
}

TrackDescription_t* backend_get_current_track(const char type)
{
    return ReportCurrentTrack(type, TRUE);
}

int backend_set_track(const char type, const int id)
{
    int ret = -1;
    if ('a' == type)
    {
        ret = SelectAudioTrack(id);
    }
    else if ('s' == type)
    {
        ret = SelectSubtitleTrack(id);
    }
    return ret;
}

#else
#error
#endif //GST_TRACKS_COMMON_FUNCTIONS
