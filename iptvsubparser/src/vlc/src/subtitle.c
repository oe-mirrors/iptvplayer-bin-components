/*****************************************************************************
 * subtitle.c: Demux for subtitle text files.
 *****************************************************************************
 * Copyright (C) 1999-2007 VLC authors and VideoLAN
 * $Id: c68821da8dbb2923c5e57e137901d31f978d2627 $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Derk-Jan Hartman <hartman at videolan dot org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h> /* isalnum() */
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <math.h>

#include "../include/subtitles.h"
#include "ttmlparser.h"

 /*****************************************************************************
 * Error values (shouldn't be exposed)
 *****************************************************************************/
#define VLC_SUCCESS        (-0) /**< No error */
#define VLC_EGENERIC       (-1) /**< Unspecified error */
#define VLC_ENOMEM         (-2) /**< Not enough memory */
#define VLC_ETIMEOUT       (-3) /**< Timeout */
#define VLC_ENOMOD         (-4) /**< Module not found */
#define VLC_ENOOBJ         (-5) /**< Object not found */
#define VLC_ENOVAR         (-6) /**< Variable not found */
#define VLC_EBADVAR        (-7) /**< Bad variable value */
#define VLC_ENOITEM        (-8) /**< Item not found */

#define VLC_UNUSED(x) (void)(x)

 /*****************************************************************************
 * Prototypes:
 *****************************************************************************/
static int SubProbeType(const char *subStr);

static int TextLoad( text_t *txt, const char *s );
static void TextUnload( text_t * );

/* Fake demux_t type */
typedef struct
{
    demux_sys_t *p_sys;
} demux_t;

static int  ParseMicroDvd   ( demux_t *, subtitle_t *, int );
static int  ParseSubRip     ( demux_t *, subtitle_t *, int );
static int  ParseSubViewer  ( demux_t *, subtitle_t *, int );
static int  ParseSSA        ( demux_t *, subtitle_t *, int );
static int  ParseVplayer    ( demux_t *, subtitle_t *, int );
static int  ParseSami       ( demux_t *, subtitle_t *, int );
static int  ParseDVDSubtitle( demux_t *, subtitle_t *, int );
static int  ParseMPL2       ( demux_t *, subtitle_t *, int );
static int  ParseAQT        ( demux_t *, subtitle_t *, int );
static int  ParsePJS        ( demux_t *, subtitle_t *, int );
static int  ParseMPSub      ( demux_t *, subtitle_t *, int );
static int  ParseJSS        ( demux_t *, subtitle_t *, int );
static int  ParsePSB        ( demux_t *, subtitle_t *, int );
static int  ParseRealText   ( demux_t *, subtitle_t *, int );
static int  ParseDKS        ( demux_t *, subtitle_t *, int );
static int  ParseSubViewer1 ( demux_t *, subtitle_t *, int );
static int  ParseCommonVTTSBV( demux_t *, subtitle_t *, int );

static const struct
{
    const char *psz_type_name;
    int  i_type;
    const char *psz_name;
    int  (*pf_read)( demux_t *, subtitle_t*, int );
} sub_read_subtitle_function [] =
{
    { "microdvd",   SUB_TYPE_MICRODVD,    "MicroDVD",    ParseMicroDvd },
    { "subrip",     SUB_TYPE_SUBRIP,      "SubRIP",      ParseSubRip },
    { "subviewer",  SUB_TYPE_SUBVIEWER,   "SubViewer",   ParseSubViewer },
    { "ssa1",       SUB_TYPE_SSA1,        "SSA-1",       ParseSSA },
    { "ssa2-4",     SUB_TYPE_SSA2_4,      "SSA-2/3/4",   ParseSSA },
    { "ass",        SUB_TYPE_ASS,         "SSA/ASS",     ParseSSA },
    { "vplayer",    SUB_TYPE_VPLAYER,     "VPlayer",     ParseVplayer },
    { "sami",       SUB_TYPE_SAMI,        "SAMI",        ParseSami },
    { "dvdsubtitle",SUB_TYPE_DVDSUBTITLE, "DVDSubtitle", ParseDVDSubtitle },
    { "mpl2",       SUB_TYPE_MPL2,        "MPL2",        ParseMPL2 },
    { "aqt",        SUB_TYPE_AQT,         "AQTitle",     ParseAQT },
    { "pjs",        SUB_TYPE_PJS,         "PhoenixSub",  ParsePJS },
    { "mpsub",      SUB_TYPE_MPSUB,       "MPSub",       ParseMPSub },
    { "jacosub",    SUB_TYPE_JACOSUB,     "JacoSub",     ParseJSS },
    { "psb",        SUB_TYPE_PSB,         "PowerDivx",   ParsePSB },
    { "realtext",   SUB_TYPE_RT,          "RealText",    ParseRealText },
    { "dks",        SUB_TYPE_DKS,         "DKS",         ParseDKS },
    { "subviewer1", SUB_TYPE_SUBVIEW1,    "Subviewer 1", ParseSubViewer1 },
    { "text/vtt",   SUB_TYPE_VTT,         "WebVTT",      ParseCommonVTTSBV },
    { "sbv",        SUB_TYPE_SBV,         "SBV",         ParseCommonVTTSBV },
    { "ttml",       SUB_TYPE_TTML,        "TTML",        NULL },
    { NULL,         SUB_TYPE_UNKNOWN,     "Unknown",     NULL }
};

static void Fix( demux_t * );

static int subtitle_cmp( const void *first, const void *second )
{
    int64_t result = ((subtitle_t *)(first))->i_start - ((subtitle_t *)(second))->i_start;
    /* Return -1, 0 ,1, and not directly substraction
     * as result can be > INT_MAX */
    return result == 0 ? 0 : result > 0 ? 1 : -1;
}
/*****************************************************************************
 * Fix: fix time stamp and order of subtitle
 *****************************************************************************/
static void Fix( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    /* *** fix order (to be sure...) *** */
    qsort( p_sys->subtitle, p_sys->i_subtitles, sizeof( p_sys->subtitle[0] ), subtitle_cmp);
}

static inline float us_strtof( const char *str, char **end )
{
    return strtof(str, end);
}

void *realloc_or_free( void *p, size_t sz )
{
    void *n = realloc(p,sz);
    if( !n )
        free(p);
    return n;
}

void strnormalize_space(char *str)
{
    char *dest = str;
    while (*str != '\0')
    {
        while (*str == ' ' && *(str + 1) == ' ')
            str++;
       *dest++ = *str++;
    }
    *dest = '\0';
}

char *strtrim(char *str, const char *whiteSpaces)
{
    size_t len = 0;
    char *frontp = str;
    char *endp = NULL;

    if( str == NULL ) { return NULL; }
    if( str[0] == '\0' ) { return str; }

    len = strlen(str);
    endp = str + len;

    while(*frontp && strchr(whiteSpaces, *frontp) ) { ++frontp; }
    if( endp != frontp )
    {
        while( strchr(whiteSpaces, *(--endp)) && endp != frontp ) {}
    }

    if( str + len - 1 != endp )
            *(endp + 1) = '\0';
    else if( frontp != str &&  endp == frontp )
            *str = '\0';

    endp = str;
    if( frontp != str )
    {
            while( *frontp ) { *endp++ = *frontp++; }
            *endp = '\0';
    }

    return str;
}

static char * peek_Readline( const char *subStr, uint64_t *pi_offset )
{
    char *psz_line = NULL;
    uint64_t i = 0;

    if (subStr[*pi_offset] == '\0')
    {
        return NULL;
    }
    else
    {
        const char *ptr = subStr + (*pi_offset);
        while ('\0' != ptr[i] && '\r' != ptr[i] && '\n' != ptr[i])
        {
            ++i;
        }

        if (0 == i && '\0' == ptr[i])
        {
            return NULL;
        }

        psz_line = malloc(i+1);
        memcpy(psz_line, ptr, i);
        psz_line[i] = '\0';

        if (ptr[i] == '\r' && ptr[i+1] == '\n')
        {
            i += 2;
        }
        else if (ptr[i] == '\r' || ptr[i] == '\n')
        {
            i += 1;
        }
    }

    *pi_offset += i;
    return psz_line;
}

static int TextLoad( text_t *txt, const char *s )
{
    int      i_line_max;
    uint64_t i_read_offset = 0;

    /* init txt */
    i_line_max          = 500;
    txt->i_line_count   = 0;
    txt->i_line         = 0;
    txt->line           = calloc( i_line_max, sizeof( char * ) );
    if( !txt->line )
        return VLC_ENOMEM;

    /* load the complete file */
    for( ;; )
    {
        char *psz = peek_Readline( s, &i_read_offset );

        if( psz == NULL )
            break;

        txt->line[txt->i_line_count++] = psz;
        if( txt->i_line_count >= i_line_max )
        {
            i_line_max += 100;
            txt->line = realloc_or_free( txt->line, i_line_max * sizeof( char * ) );
            if( !txt->line )
                return VLC_ENOMEM;
        }
    }

    if( txt->i_line_count <= 0 )
    {
        free( txt->line );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static void TextUnload( text_t *txt )
{
    int i;

    for( i = 0; i < txt->i_line_count; i++ )
    {
        free( txt->line[i] );
    }
    free( txt->line );
    txt->i_line       = 0;
    txt->i_line_count = 0;
}

static char *TextGetLine( text_t *txt )
{
    if( txt->i_line >= txt->i_line_count )
        return( NULL );

    return txt->line[txt->i_line++];
}
static void TextPreviousLine( text_t *txt )
{
    if( txt->i_line > 0 )
        txt->i_line--;
}

static int SubProbeType(const char *subStr)
{
    int     i_type = SUB_TYPE_UNKNOWN;
    int     i_try;
    char    *s = NULL;
    uint64_t i_read_offset = 0;

    for( i_try = 0; i_try < 256; i_try++ )
    {
        int i_dummy;
        char p_dummy;

        if( (s = peek_Readline( subStr, &i_read_offset )) == NULL )
            break;

        if( strcasestr( s, "<SAMI>" ) )
        {
            i_type = SUB_TYPE_SAMI;
            break;
        }
        else if( sscanf( s, "{%d}{%d}", &i_dummy, &i_dummy ) == 2 ||
                 sscanf( s, "{%d}{}", &i_dummy ) == 1)
        {
            i_type = SUB_TYPE_MICRODVD;
            break;
        }
        else if( sscanf( s, "%d:%d:%d,%d --> %d:%d:%d,%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy ) == 8 ||
                 sscanf( s, "%d:%d:%d --> %d:%d:%d,%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy ) == 7 ||
                 sscanf( s, "%d:%d:%d,%d --> %d:%d:%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy ) == 7 ||
                 sscanf( s, "%d:%d:%d.%d --> %d:%d:%d.%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy ) == 8 ||
                 sscanf( s, "%d:%d:%d --> %d:%d:%d.%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy ) == 7 ||
                 sscanf( s, "%d:%d:%d.%d --> %d:%d:%d",
                         &i_dummy,&i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy ) == 7 ||
                 sscanf( s, "%d:%d:%d --> %d:%d:%d",
                         &i_dummy,&i_dummy,&i_dummy,
                         &i_dummy,&i_dummy,&i_dummy ) == 6 )
        {
            i_type = SUB_TYPE_SUBRIP;
            break;
        }
        else if( !strncasecmp( s, "!: This is a Sub Station Alpha v1", 33 ) )
        {
            i_type = SUB_TYPE_SSA1;
            break;
        }
        else if( !strncasecmp( s, "ScriptType: v4.00+", 18 ) )
        {
            i_type = SUB_TYPE_ASS;
            break;
        }
        else if( !strncasecmp( s, "ScriptType: v4.00", 17 ) )
        {
            i_type = SUB_TYPE_SSA2_4;
            break;
        }
        else if( !strncasecmp( s, "Dialogue: Marked", 16  ) )
        {
            i_type = SUB_TYPE_SSA2_4;
            break;
        }
        else if( !strncasecmp( s, "Dialogue:", 9  ) )
        {
            i_type = SUB_TYPE_ASS;
            break;
        }
        else if( strcasestr( s, "[INFORMATION]" ) )
        {
            i_type = SUB_TYPE_SUBVIEWER; /* I hope this will work */
            break;
        }
        else if( sscanf( s, "%d:%d:%d.%d %d:%d:%d",
                             &i_dummy, &i_dummy, &i_dummy, &i_dummy,
                             &i_dummy, &i_dummy, &i_dummy ) == 7 ||
                 sscanf( s, "@%d @%d", &i_dummy, &i_dummy) == 2)
        {
            i_type = SUB_TYPE_JACOSUB;
            break;
        }
        else if( sscanf( s, "%d:%d:%d.%d,%d:%d:%d.%d",
                             &i_dummy, &i_dummy, &i_dummy, &i_dummy,
                             &i_dummy, &i_dummy, &i_dummy, &i_dummy ) == 8 )
        {
            i_type = SUB_TYPE_SBV;
            break;
        }
        else if( sscanf( s, "%d:%d:%d:", &i_dummy, &i_dummy, &i_dummy ) == 3 ||
                 sscanf( s, "%d:%d:%d ", &i_dummy, &i_dummy, &i_dummy ) == 3 )
        {
            i_type = SUB_TYPE_VPLAYER;
            break;
        }
        else if( sscanf( s, "{T %d:%d:%d:%d", &i_dummy, &i_dummy,
                         &i_dummy, &i_dummy ) == 4 )
        {
            i_type = SUB_TYPE_DVDSUBTITLE;
            break;
        }
        else if( sscanf( s, "[%d:%d:%d]%c",
                 &i_dummy, &i_dummy, &i_dummy, &p_dummy ) == 4 )
        {
            i_type = SUB_TYPE_DKS;
            break;
        }
        else if( strstr( s, "*** START SCRIPT" ) )
        {
            i_type = SUB_TYPE_SUBVIEW1;
            break;
        }
        else if( sscanf( s, "[%d][%d]", &i_dummy, &i_dummy ) == 2 ||
                 sscanf( s, "[%d][]", &i_dummy ) == 1)
        {
            i_type = SUB_TYPE_MPL2;
            break;
        }
        else if( sscanf (s, "FORMAT=%d", &i_dummy) == 1 ||
                 ( sscanf (s, "FORMAT=TIM%c", &p_dummy) == 1
                   && p_dummy =='E' ) )
        {
            i_type = SUB_TYPE_MPSUB;
            break;
        }
        else if( sscanf( s, "-->> %d", &i_dummy) == 1 )
        {
            i_type = SUB_TYPE_AQT;
            break;
        }
        else if( sscanf( s, "%d,%d,", &i_dummy, &i_dummy ) == 2 )
        {
            i_type = SUB_TYPE_PJS;
            break;
        }
        else if( sscanf( s, "{%d:%d:%d}",
                            &i_dummy, &i_dummy, &i_dummy ) == 3 )
        {
            i_type = SUB_TYPE_PSB;
            break;
        }
        else if( strcasestr( s, "<time" ) )
        {
            i_type = SUB_TYPE_RT;
            break;
        }
        else if( !strncasecmp( s, "WEBVTT", 6 ) )
        {
            i_type = SUB_TYPE_VTT;
            break;
        }
        else if( strcasestr( s, "/ttml" ) || strcasestr( s, "/ttaf" ) || strcasestr( s, "<tt " ) )
        {
            i_type = SUB_TYPE_TTML;
            break;
        }

        if (NULL != s)
        {
            free(s);
            s = NULL;
        }
    }

    if (NULL != s)
    {
        free(s);
        s = NULL;
    }

    return i_type;
}


int VLC_SubtitleDemuxOpen( const char *subStr, const int i_microsecperframe, demux_sys_t **pp_sys )
{
    demux_sys_t *p_sys;
    int  (*pf_read)( demux_t *, subtitle_t*, int );
    int  i, i_max;
    demux_t demux;

    memset(&demux, 0, sizeof(demux));

    p_sys = calloc( sizeof( demux_sys_t ), 1 );
    if( p_sys == NULL )
        return VLC_ENOMEM;
    demux.p_sys = p_sys;

    p_sys->psz_header         = NULL;
    p_sys->i_subtitle         = 0;
    p_sys->i_subtitles        = 0;
    p_sys->subtitle           = NULL;
    p_sys->i_microsecperframe = i_microsecperframe; //40000;

    p_sys->jss.b_inited       = false;
    p_sys->mpsub.b_inited     = false;

    p_sys->i_type = SubProbeType( subStr );

    /* Quit on unknown subtitles */
    if( p_sys->i_type == SUB_TYPE_UNKNOWN )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    for( i = 0; ; i++ )
    {
        if( sub_read_subtitle_function[i].i_type == p_sys->i_type )
        {
            // fprintf(stderr, "Parser [%s] will be used.", sub_read_subtitle_function[i].psz_name);
            p_sys->psz_type_name = strdup(sub_read_subtitle_function[i].psz_type_name);
            pf_read = sub_read_subtitle_function[i].pf_read;
            break;
        }
    }

    if ( p_sys->i_type == SUB_TYPE_TTML  )
    {
        int status = ReadSubtitltesTTML(p_sys, subStr);
        if (status != VLC_SUCCESS)
        {
            VLC_SubtitleDemuxClose(p_sys);
            return status;
        }
    }
    else
    {
        /* Load the whole file */
        TextLoad( &p_sys->txt, subStr );

        /* Parse it */
        for( i_max = 0;; )
        {
            if( p_sys->i_subtitles >= i_max )
            {
                i_max += 500;
                if( !( p_sys->subtitle = realloc_or_free( p_sys->subtitle,
                                                  sizeof(subtitle_t) * i_max ) ) )
                {
                    TextUnload( &p_sys->txt );
                    free( p_sys->psz_type_name );
                    free( p_sys );
                    return VLC_ENOMEM;
                }
            }

            if( pf_read( &demux, &p_sys->subtitle[p_sys->i_subtitles],
                         p_sys->i_subtitles ) )
                break;

            p_sys->i_subtitles++;
        }

        /* Unload */
        TextUnload( &p_sys->txt );
    }

    /* Fix subtitle (order and time) *** */
    p_sys->i_subtitle = 0;
    p_sys->i_length = 0;
    if( p_sys->i_subtitles > 0 )
    {
        p_sys->i_length = p_sys->subtitle[p_sys->i_subtitles-1].i_stop;
        /* +1 to avoid 0 */
        if( p_sys->i_length <= 0 )
            p_sys->i_length = p_sys->subtitle[p_sys->i_subtitles-1].i_start+1;
    }

    /*
    if( p_sys->i_type == SUB_TYPE_SSA1 ||
             p_sys->i_type == SUB_TYPE_SSA2_4 ||
             p_sys->i_type == SUB_TYPE_ASS )
    */
    {
        Fix( &demux );
    }

    *pp_sys = p_sys;
    return VLC_SUCCESS;
}

void VLC_SubtitleDemuxClose( demux_sys_t *p_sys )
{
    if (p_sys->subtitle)
    {
        int i;
        for( i = 0; i < p_sys->i_subtitles; i++ )
        {
            free( p_sys->subtitle[i].psz_text );
        }
    }

    free( p_sys->psz_type_name );
    free( p_sys->subtitle );
    free( p_sys->psz_header );
    free( p_sys );
}

/*****************************************************************************
 * Specific Subtitle function
 *****************************************************************************/
/* ParseMicroDvd:
 *  Format:
 *      {n1}{n2}Line1|Line2|Line3....
 *  where n1 and n2 are the video frame number (n2 can be empty)
 */
static int ParseMicroDvd( demux_t *p_demux, subtitle_t *p_subtitle,
                          int i_idx )
{
    VLC_UNUSED( i_idx );
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int  i_start;
    int  i_stop;
    int  i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        i_start = 0;
        i_stop  = -1;
        if( sscanf( s, "{%d}{}%[^\r\n]", &i_start, psz_text ) == 2 ||
            sscanf( s, "{%d}{%d}%[^\r\n]", &i_start, &i_stop, psz_text ) == 3)
        {
            if( i_start != 1 || i_stop != 1 )
                break;

            /* We found a possible setting of the framerate "{1}{1}23.976" */
            /* Check if it's usable, and if the sub-fps is not set */
            float f_fps = us_strtof( psz_text, NULL );
            if( f_fps > 0.f && 0 == p_sys->i_microsecperframe )
                p_sys->i_microsecperframe = llroundf(1000000.f / f_fps);
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }

    /* */
    p_subtitle->i_start  = i_start * p_sys->i_microsecperframe;
    p_subtitle->i_stop   = i_stop >= 0 ? (i_stop  * p_sys->i_microsecperframe) : -1;
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

/* ParseSubRipSubViewer
 *  Format SubRip
 *      n
 *      h1:m1:s1,d1 --> h2:m2:s2,d2
 *      Line1
 *      Line2
 *      ....
 *      [Empty line]
 *  Format SubViewer v1/v2
 *      h1:m1:s1.d1,h2:m2:s2.d2
 *      Line1[br]Line2
 *      Line3
 *      ...
 *      [empty line]
 *  We ignore line number for SubRip
 */
static int ParseSubRipSubViewer( demux_t *p_demux, subtitle_t *p_subtitle,
                                 int (* pf_parse_timing)(subtitle_t *, const char *),
                                 bool b_replace_br )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char    *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        if( pf_parse_timing( p_subtitle, s) == VLC_SUCCESS &&
            p_subtitle->i_start < p_subtitle->i_stop )
        {
            break;
        }
    }

    /* Now read text until an empty line */
    psz_text = strdup("");
    if( !psz_text )
        return VLC_ENOMEM;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_len;
        int i_old;

        i_len = s ? strlen( s ) : 0;
        if( i_len <= 0 )
        {
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }

        i_old = strlen( psz_text );
        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
        {
            return VLC_ENOMEM;
        }
        strcat( psz_text, s );
        strcat( psz_text, "\n" );

        /* replace [br] by \n */
        if( b_replace_br )
        {
            char *p;

            while( ( p = strstr( psz_text, "[br]" ) ) )
            {
                *p++ = '\n';
                memmove( p, &p[3], strlen(&p[3])+1 );
            }
        }
    }
}

/* subtitle_ParseSubRipTimingValue
 * Parses SubRip timing value.
 */
static int subtitle_ParseSubRipTimingValue(int64_t *timing_value, const char *s)
{
    int h1, m1, s1, d1 = 0;

    if ( sscanf( s, "%d:%d:%d,%d",
                 &h1, &m1, &s1, &d1 ) == 4 ||
         sscanf( s, "%d:%d:%d.%d",
                 &h1, &m1, &s1, &d1 ) == 4 ||
         sscanf( s, "%d:%d:%d",
                 &h1, &m1, &s1) == 3 )
    {
        (*timing_value) = ( (int64_t)h1 * 3600 * 1000 +
                            (int64_t)m1 * 60 * 1000 +
                            (int64_t)s1 * 1000 +
                            (int64_t)d1 ) * 1000;

        return VLC_SUCCESS;
    }

    return VLC_EGENERIC;
}

/* subtitle_ParseSubRipTiming
 * Parses SubRip timing.
 */
static int subtitle_ParseSubRipTiming( subtitle_t *p_subtitle,
                                       const char *s )
{
    int i_result = VLC_EGENERIC;
    char *psz_start, *psz_stop;
    psz_start = malloc( strlen(s) + 1 );
    psz_stop = malloc( strlen(s) + 1 );

    if( sscanf( s, "%s --> %s", psz_start, psz_stop) == 2 &&
        subtitle_ParseSubRipTimingValue( &p_subtitle->i_start, psz_start ) == VLC_SUCCESS &&
        subtitle_ParseSubRipTimingValue( &p_subtitle->i_stop,  psz_stop ) == VLC_SUCCESS )
    {
        i_result = VLC_SUCCESS;
    }

    free(psz_start);
    free(psz_stop);

    return i_result;
}
/* ParseSubRip
 */
static int  ParseSubRip( demux_t *p_demux, subtitle_t *p_subtitle,
                         int i_idx )
{
    VLC_UNUSED( i_idx );
    return ParseSubRipSubViewer( p_demux, p_subtitle,
                                 &subtitle_ParseSubRipTiming,
                                 false );
}

/* subtitle_ParseSubViewerTiming
 * Parses SubViewer timing.
 */
static int subtitle_ParseSubViewerTiming( subtitle_t *p_subtitle,
                                   const char *s )
{
    int h1, m1, s1, d1, h2, m2, s2, d2;

    if( sscanf( s, "%d:%d:%d.%d,%d:%d:%d.%d",
                &h1, &m1, &s1, &d1, &h2, &m2, &s2, &d2) == 8 )
    {
        p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                (int64_t)m1 * 60*1000 +
                                (int64_t)s1 * 1000 +
                                (int64_t)d1 ) * 1000;

        p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                (int64_t)m2 * 60*1000 +
                                (int64_t)s2 * 1000 +
                                (int64_t)d2 ) * 1000;
        return VLC_SUCCESS;
    }
    return VLC_EGENERIC;
}

/* ParseSubViewer
 */
static int  ParseSubViewer( demux_t *p_demux, subtitle_t *p_subtitle,
                            int i_idx )
{
    VLC_UNUSED( i_idx );

    return ParseSubRipSubViewer( p_demux, p_subtitle,
                                 &subtitle_ParseSubViewerTiming,
                                 true );
}

/* ParseSSA
 */
static int  ParseSSA( demux_t *p_demux, subtitle_t *p_subtitle,
                      int i_idx )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    size_t header_len = 0;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1, c1, h2, m2, s2, c2;
        char *psz_text, *psz_temp;
        char temp[16];

        if( !s )
            return VLC_EGENERIC;

        /* We expect (SSA2-4):
         * Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
         * Dialogue: Marked=0,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
         *
         * SSA-1 is similar but only has 8 commas up untill the subtitle text. Probably the Effect field is no present, but not 100 % sure.
         */

        /* For ASS:
         * Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
         * Dialogue: Layer#,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
         */

        /* The output text is - at least, not removing numbers - 18 chars shorter than the input text. */
        psz_text = malloc( strlen(s) );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s,
                    "Dialogue: %15[^,],%d:%d:%d.%d,%d:%d:%d.%d,%[^\r\n]",
                    temp,
                    &h1, &m1, &s1, &c1,
                    &h2, &m2, &s2, &c2,
                    psz_text ) == 10 )
        {
            /* The dec expects: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text */
            /* (Layer comes from ASS specs ... it's empty for SSA.) */
            if( p_sys->i_type == SUB_TYPE_SSA1 )
            {
                /* SSA1 has only 8 commas before the text starts, not 9 */
                memmove( &psz_text[1], psz_text, strlen(psz_text)+1 );
                psz_text[0] = ',';
            }
            else
            {
                int i_layer = ( p_sys->i_type == SUB_TYPE_ASS ) ? atoi( temp ) : 0;

                /* ReadOrder, Layer, %s(rest of fields) */
                if( asprintf( &psz_temp, "%d,%d,%s", i_idx, i_layer, psz_text ) == -1 )
                {
                    free( psz_text );
                    return VLC_ENOMEM;
                }

                free( psz_text );
                psz_text = psz_temp;
            }

            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)c1 * 10 ) * 1000;
            p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                    (int64_t)m2 * 60*1000 +
                                    (int64_t)s2 * 1000 +
                                    (int64_t)c2 * 10 ) * 1000;
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }
        free( psz_text );

        /* All the other stuff we add to the header field */
        if( header_len == 0 && p_sys->psz_header )
            header_len = strlen( p_sys->psz_header );

        size_t s_len = strlen( s );
        p_sys->psz_header = realloc_or_free( p_sys->psz_header, header_len + s_len + 2 );
        if( !p_sys->psz_header )
            return VLC_ENOMEM;
        snprintf( p_sys->psz_header + header_len, s_len + 2, "%s\n", s );
        header_len += s_len + 1;
    }
}

/* ParseVplayer
 *  Format
 *      h:m:s:Line1|Line2|Line3....
 *  or
 *      h:m:s Line1|Line2|Line3....
 */
static int ParseVplayer( demux_t *p_demux, subtitle_t *p_subtitle,
                          int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s, "%d:%d:%d%*c%[^\r\n]",
                    &h1, &m1, &s1, psz_text ) == 4 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 ) * 1000;
            p_subtitle->i_stop  = -1;
            break;
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

/* ParseSami
 */
static char *ParseSamiSearch( text_t *txt,
                              char *psz_start, const char *psz_str )
{
    if( psz_start && strcasestr( psz_start, psz_str ) )
    {
        char *s = strcasestr( psz_start, psz_str );
        return &s[strlen( psz_str )];
    }

    for( ;; )
    {
        char *p = TextGetLine( txt );
        if( !p )
            return NULL;

        if( strcasestr( p, psz_str ) )
        {
            char *s = strcasestr( p, psz_str );
            return &s[strlen( psz_str )];
        }
    }
}
static int  ParseSami( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;

    char *s;
    int64_t i_start;

    unsigned int i_text;
    char text[8192]; /* Arbitrary but should be long enough */

    /* search "Start=" */
    if( !( s = ParseSamiSearch( txt, NULL, "Start=" ) ) )
        return VLC_EGENERIC;

    /* get start value */
    i_start = strtol( s, &s, 0 );

    /* search <P */
    if( !( s = ParseSamiSearch( txt, s, "<P" ) ) )
        return VLC_EGENERIC;

    /* search > */
    if( !( s = ParseSamiSearch( txt, s, ">" ) ) )
        return VLC_EGENERIC;

    i_text = 0;
    text[0] = '\0';
    /* now get all txt until  a "Start=" line */
    for( ;; )
    {
        char c = '\0';
        /* Search non empty line */
        while( s && *s == '\0' )
            s = TextGetLine( txt );
        if( !s )
            break;

        if( *s == '<' )
        {
            if( !strncasecmp( s, "<br", 3 ) )
            {
                c = '\n';
            }
            else if( strcasestr( s, "Start=" ) )
            {
                TextPreviousLine( txt );
                break;
            }
            s = ParseSamiSearch( txt, s, ">" );
        }
        else if( !strncmp( s, "&nbsp;", 6 ) )
        {
            c = ' ';
            s += 6;
        }
        else if( *s == '\t' )
        {
            c = ' ';
            s++;
        }
        else
        {
            c = *s;
            s++;
        }
        if( c != '\0' && i_text+1 < sizeof(text) )
        {
            text[i_text++] = c;
            text[i_text] = '\0';
        }
    }

    p_subtitle->i_start = i_start * 1000;
    p_subtitle->i_stop  = -1;
    p_subtitle->psz_text = strdup( text );

    return VLC_SUCCESS;
}

/* ParseDVDSubtitle
 *  Format
 *      {T h1:m1:s1:c1
 *      Line1
 *      Line2
 *      ...
 *      }
 * TODO it can have a header
 *      { HEAD
 *          ...
 *          CODEPAGE=...
 *          FORMAT=...
 *          LANG=English
 *      }
 *      LANG support would be cool
 *      CODEPAGE is probably mandatory FIXME
 */
static int ParseDVDSubtitle( demux_t *p_demux, subtitle_t *p_subtitle,
                             int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1, m1, s1, c1;

        if( !s )
            return VLC_EGENERIC;

        if( sscanf( s,
                    "{T %d:%d:%d:%d",
                    &h1, &m1, &s1, &c1 ) == 4 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)c1 * 10) * 1000;
            p_subtitle->i_stop = -1;
            break;
        }
    }

    /* Now read text until a line containing "}" */
    psz_text = strdup("");
    if( !psz_text )
        return VLC_ENOMEM;
    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_len;
        int i_old;

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        i_len = strlen( s );
        if( i_len == 1 && s[0] == '}')
        {
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }

        i_old = strlen( psz_text );
        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
            return VLC_ENOMEM;
        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }
}

/* ParseMPL2
 *  Format
 *     [n1][n2]Line1|Line2|Line3...
 *  where n1 and n2 are the video frame number (n2 can be empty)
 */
static int ParseMPL2( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_start;
        int i_stop;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        i_start = 0;
        i_stop  = -1;
        if( sscanf( s, "[%d][] %[^\r\n]", &i_start, psz_text ) == 2 ||
            sscanf( s, "[%d][%d] %[^\r\n]", &i_start, &i_stop, psz_text ) == 3)
        {
            p_subtitle->i_start = (int64_t)i_start * 100000;
            p_subtitle->i_stop  = i_stop >= 0 ? ((int64_t)i_stop  * 100000) : -1;
            break;
        }
        free( psz_text );
    }

    for( i = 0; psz_text[i] != '\0'; )
    {
        /* replace | by \n */
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';

        /* Remove italic */
        if( psz_text[i] == '/' && ( i == 0 || psz_text[i-1] == '\n' ) )
            memmove( &psz_text[i], &psz_text[i+1], strlen(&psz_text[i+1])+1 );
        else
            i++;
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseAQT( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text = strdup( "" );
    int i_old = 0;
    int i_firstline = 1;

    for( ;; )
    {
        int t; /* Time */

        const char *s = TextGetLine( txt );

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        /* Data Lines */
        if( sscanf (s, "-->> %d", &t) == 1)
        {
            p_subtitle->i_start = (int64_t)t; /* * FPS*/
            p_subtitle->i_stop  = -1;

            /* Starting of a subtitle */
            if( i_firstline )
            {
                i_firstline = 0;
            }
            /* We have been too far: end of the subtitle, begin of next */
            else
            {
                TextPreviousLine( txt );
                break;
            }
        }
        /* Text Lines */
        else
        {
            i_old = strlen( psz_text ) + 1;
            psz_text = realloc_or_free( psz_text, i_old + strlen( s ) + 1 );
            if( !psz_text )
                 return VLC_ENOMEM;
            strcat( psz_text, s );
            strcat( psz_text, "\n" );
            if( txt->i_line == txt->i_line_count )
                break;
        }
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParsePJS( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int t1, t2;

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen(s) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        /* Data Lines */
        if( sscanf (s, "%d,%d,\"%[^\n\r]", &t1, &t2, psz_text ) == 3 )
        {
            /* 1/10th of second ? Frame based ? FIXME */
            p_subtitle->i_start = 10 * t1;
            p_subtitle->i_stop = 10 * t2;
            /* Remove latest " */
            psz_text[ strlen(psz_text) - 1 ] = '\0';

            break;
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }

    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseMPSub( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text = strdup( "" );

    if( !p_sys->mpsub.b_inited )
    {
        p_sys->mpsub.f_total = 0.0;
        p_sys->mpsub.f_factor = 0.0;

        p_sys->mpsub.b_inited = true;
    }

    for( ;; )
    {
        char p_dummy;
        char *psz_temp;

        const char *s = TextGetLine( txt );
        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        if( strstr( s, "FORMAT" ) )
        {
            if( sscanf (s, "FORMAT=TIM%c", &p_dummy ) == 1 && p_dummy == 'E')
            {
                p_sys->mpsub.f_factor = 100.0;
                break;
            }

            psz_temp = malloc( strlen(s) );
            if( !psz_temp )
            {
                free( psz_text );
                return VLC_ENOMEM;
            }

            if( sscanf( s, "FORMAT=%[^\r\n]", psz_temp ) )
            {
                p_sys->mpsub.f_factor = 1.f;
                free( psz_temp );
                break;
            }
            free( psz_temp );
        }

        /* Data Lines */
        float f1 = us_strtof( s, &psz_temp );
        if( *psz_temp )
        {
            float f2 = us_strtof( psz_temp, NULL );
            p_sys->mpsub.f_total += f1 * p_sys->mpsub.f_factor;
            p_subtitle->i_start = llroundf(10000.f * p_sys->mpsub.f_total);
            p_sys->mpsub.f_total += f2 * p_sys->mpsub.f_factor;
            p_subtitle->i_stop = llroundf(10000.f * p_sys->mpsub.f_total);
            break;
        }
    }

    for( ;; )
    {
        const char *s = TextGetLine( txt );

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        int i_len = strlen( s );
        if( i_len == 0 )
            break;

        int i_old = strlen( psz_text );

        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
             return VLC_ENOMEM;

        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }

    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseJSS( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t  *p_sys = p_demux->p_sys;
    text_t       *txt = &p_sys->txt;
    char         *psz_text, *psz_orig;
    char         *psz_text2, *psz_orig2;
    int h1, h2, m1, m2, s1, s2, f1, f2;

    if( !p_sys->jss.b_inited )
    {
        p_sys->jss.i_comment = 0;
        p_sys->jss.i_time_resolution = 30;
        p_sys->jss.i_time_shift = 0;

        p_sys->jss.b_inited = true;
    }

    /* Parse the main lines */
    for( ;; )
    {
        const char *s = TextGetLine( txt );
        if( !s )
            return VLC_EGENERIC;

        psz_orig = malloc( strlen( s ) + 1 );
        if( !psz_orig )
            return VLC_ENOMEM;
        psz_text = psz_orig;

        /* Complete time lines */
        if( sscanf( s, "%d:%d:%d.%d %d:%d:%d.%d %[^\n\r]",
                    &h1, &m1, &s1, &f1, &h2, &m2, &s2, &f2, psz_text ) == 9 )
        {
            p_subtitle->i_start = ( (int64_t)( h1 *3600 + m1 * 60 + s1 ) +
                (int64_t)( ( f1 +  p_sys->jss.i_time_shift ) /  p_sys->jss.i_time_resolution ) )
                * 1000000;
            p_subtitle->i_stop = ( (int64_t)( h2 *3600 + m2 * 60 + s2 ) +
                (int64_t)( ( f2 +  p_sys->jss.i_time_shift ) /  p_sys->jss.i_time_resolution ) )
                * 1000000;
            break;
        }
        /* Short time lines */
        else if( sscanf( s, "@%d @%d %[^\n\r]", &f1, &f2, psz_text ) == 3 )
        {
            p_subtitle->i_start = (int64_t)(
                    ( f1 + p_sys->jss.i_time_shift ) / p_sys->jss.i_time_resolution * 1000000.0 );
            p_subtitle->i_stop = (int64_t)(
                    ( f2 + p_sys->jss.i_time_shift ) / p_sys->jss.i_time_resolution * 1000000.0 );
            break;
        }
        /* General Directive lines */
        /* Only TIME and SHIFT are supported so far */
        else if( s[0] == '#' )
        {
            int h = 0, m =0, sec = 1, f = 1;
            unsigned shift = 1;
            int inv = 1;

            strcpy( psz_text, s );

            switch( toupper( (unsigned char)psz_text[1] ) )
            {
            case 'S':
                 shift = isalpha( (unsigned char)psz_text[2] ) ? 6 : 2 ;

                 if( sscanf( &psz_text[shift], "%d", &h ) )
                 {
                     /* Negative shifting */
                     if( h < 0 )
                     {
                         h *= -1;
                         inv = -1;
                     }

                     if( sscanf( &psz_text[shift], "%*d:%d", &m ) )
                     {
                         if( sscanf( &psz_text[shift], "%*d:%*d:%d", &sec ) )
                         {
                             sscanf( &psz_text[shift], "%*d:%*d:%*d.%d", &f );
                         }
                         else
                         {
                             h = 0;
                             sscanf( &psz_text[shift], "%d:%d.%d",
                                     &m, &sec, &f );
                             m *= inv;
                         }
                     }
                     else
                     {
                         h = m = 0;
                         sscanf( &psz_text[shift], "%d.%d", &sec, &f);
                         sec *= inv;
                     }
                     p_sys->jss.i_time_shift = ( ( h * 3600 + m * 60 + sec )
                         * p_sys->jss.i_time_resolution + f ) * inv;
                 }
                 break;

            case 'T':
                shift = isalpha( (unsigned char)psz_text[2] ) ? 8 : 2 ;

                sscanf( &psz_text[shift], "%d", &p_sys->jss.i_time_resolution );
                break;
            }
            free( psz_orig );
            continue;
        }
        else
            /* Unkown type line, probably a comment */
        {
            free( psz_orig );
            continue;
        }
    }

    while( psz_text[ strlen( psz_text ) - 1 ] == '\\' )
    {
        const char *s2 = TextGetLine( txt );

        if( !s2 )
        {
            free( psz_orig );
            return VLC_EGENERIC;
        }

        int i_len = strlen( s2 );
        if( i_len == 0 )
            break;

        int i_old = strlen( psz_text );

        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 );
        if( !psz_text )
             return VLC_ENOMEM;

        psz_orig = psz_text;
        strcat( psz_text, s2 );
    }

    /* Skip the blanks */
    while( *psz_text == ' ' || *psz_text == '\t' ) psz_text++;

    /* Parse the directives */
    if( isalpha( (unsigned char)*psz_text ) || *psz_text == '[' )
    {
        while( *psz_text != ' ' )
        { psz_text++ ;};

        /* Directives are NOT parsed yet */
        /* This has probably a better place in a decoder ? */
        /* directive = malloc( strlen( psz_text ) + 1 );
           if( sscanf( psz_text, "%s %[^\n\r]", directive, psz_text2 ) == 2 )*/
    }

    /* Skip the blanks after directives */
    while( *psz_text == ' ' || *psz_text == '\t' ) psz_text++;

    /* Clean all the lines from inline comments and other stuffs */
    psz_orig2 = calloc( strlen( psz_text) + 1, 1 );
    psz_text2 = psz_orig2;

    for( ; *psz_text != '\0' && *psz_text != '\n' && *psz_text != '\r'; )
    {
        switch( *psz_text )
        {
        case '{':
            p_sys->jss.i_comment++;
            break;
        case '}':
            if( p_sys->jss.i_comment )
            {
                p_sys->jss.i_comment = 0;
                if( (*(psz_text + 1 ) ) == ' ' ) psz_text++;
            }
            break;
        case '~':
            if( !p_sys->jss.i_comment )
            {
                *psz_text2 = ' ';
                psz_text2++;
            }
            break;
        case ' ':
        case '\t':
            if( (*(psz_text + 1 ) ) == ' ' || (*(psz_text + 1 ) ) == '\t' )
                break;
            if( !p_sys->jss.i_comment )
            {
                *psz_text2 = ' ';
                psz_text2++;
            }
            break;
        case '\\':
            if( (*(psz_text + 1 ) ) == 'n' )
            {
                *psz_text2 = '\n';
                psz_text++;
                psz_text2++;
                break;
            }
            if( ( toupper((unsigned char)*(psz_text + 1 ) ) == 'C' ) ||
                    ( toupper((unsigned char)*(psz_text + 1 ) ) == 'F' ) )
            {
                psz_text++; psz_text++;
                break;
            }
            if( (*(psz_text + 1 ) ) == 'B' || (*(psz_text + 1 ) ) == 'b' ||
                (*(psz_text + 1 ) ) == 'I' || (*(psz_text + 1 ) ) == 'i' ||
                (*(psz_text + 1 ) ) == 'U' || (*(psz_text + 1 ) ) == 'u' ||
                (*(psz_text + 1 ) ) == 'D' || (*(psz_text + 1 ) ) == 'N' )
            {
                psz_text++;
                break;
            }
            if( (*(psz_text + 1 ) ) == '~' || (*(psz_text + 1 ) ) == '{' ||
                (*(psz_text + 1 ) ) == '\\' )
                psz_text++;
            else if( *(psz_text + 1 ) == '\r' ||  *(psz_text + 1 ) == '\n' ||
                     *(psz_text + 1 ) == '\0' )
            {
                psz_text++;
            }
            break;
        default:
            if( !p_sys->jss.i_comment )
            {
                *psz_text2 = *psz_text;
                psz_text2++;
            }
        }
        psz_text++;
    }

    p_subtitle->psz_text = psz_orig2;
    free( psz_orig );
    return VLC_SUCCESS;
}

static int ParsePSB( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;
    int i;

    for( ;; )
    {
        int h1, m1, s1;
        int h2, m2, s2;
        const char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s, "{%d:%d:%d}{%d:%d:%d}%[^\r\n]",
                    &h1, &m1, &s1, &h2, &m2, &s2, psz_text ) == 7 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 ) * 1000;
            p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                    (int64_t)m2 * 60*1000 +
                                    (int64_t)s2 * 1000 ) * 1000;
            break;
        }
        free( psz_text );
    }

    /* replace | by \n */
    for( i = 0; psz_text[i] != '\0'; i++ )
    {
        if( psz_text[i] == '|' )
            psz_text[i] = '\n';
    }
    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int64_t ParseRealTime( char *psz, int *h, int *m, int *s, int *f )
{
    if( *psz == '\0' ) return 0;
    if( sscanf( psz, "%d:%d:%d.%d", h, m, s, f ) == 4 ||
            sscanf( psz, "%d:%d.%d", m, s, f ) == 3 ||
            sscanf( psz, "%d.%d", s, f ) == 2 ||
            sscanf( psz, "%d:%d", m, s ) == 2 ||
            sscanf( psz, "%d", s ) == 1 )
    {
        return (int64_t)((( *h * 60 + *m ) * 60 ) + *s ) * 1000 * 1000
               + (int64_t)*f * 10 * 1000;
    }
    else return VLC_EGENERIC;
}

static int ParseRealText( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text = NULL;

    for( ;; )
    {
        int h1 = 0, m1 = 0, s1 = 0, f1 = 0;
        int h2 = 0, m2 = 0, s2 = 0, f2 = 0;
        const char *s = TextGetLine( txt );
        free( psz_text );

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        /* Find the good begining. This removes extra spaces at the beginning
           of the line.*/
        char *psz_temp = strcasestr( s, "<time");
        if( psz_temp != NULL )
        {
            char psz_end[12], psz_begin[12];
            /* Line has begin and end */
            if( ( sscanf( psz_temp,
                  "<%*[t|T]ime %*[b|B]egin=\"%11[^\"]\" %*[e|E]nd=\"%11[^\"]%*[^>]%[^\n\r]",
                            psz_begin, psz_end, psz_text) != 3 ) &&
                    /* Line has begin and no end */
                    ( sscanf( psz_temp,
                              "<%*[t|T]ime %*[b|B]egin=\"%11[^\"]\"%*[^>]%[^\n\r]",
                              psz_begin, psz_text ) != 2) )
                /* Line is not recognized */
            {
                continue;
            }

            /* Get the times */
            int64_t i_time = ParseRealTime( psz_begin, &h1, &m1, &s1, &f1 );
            p_subtitle->i_start = i_time >= 0 ? i_time : 0;

            i_time = ParseRealTime( psz_end, &h2, &m2, &s2, &f2 );
            p_subtitle->i_stop = i_time >= 0 ? i_time : -1;
            break;
        }
    }

    /* Get the following Lines */
    for( ;; )
    {
        const char *s = TextGetLine( txt );

        if( !s )
        {
            free( psz_text );
            return VLC_EGENERIC;
        }

        int i_len = strlen( s );
        if( i_len == 0 ) break;

        if( strcasestr( s, "<time" ) ||
            strcasestr( s, "<clear/") )
        {
            TextPreviousLine( txt );
            break;
        }

        int i_old = strlen( psz_text );

        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }

    /* Remove the starting ">" that remained after the sscanf */
    memmove( &psz_text[0], &psz_text[1], strlen( psz_text ) );

    p_subtitle->psz_text = psz_text;

    return VLC_SUCCESS;
}

static int ParseDKS( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;

    for( ;; )
    {
        int h1, m1, s1;
        int h2, m2, s2;
        char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        psz_text = malloc( strlen( s ) + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        if( sscanf( s, "[%d:%d:%d]%[^\r\n]",
                    &h1, &m1, &s1, psz_text ) == 4 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 ) * 1000;

            char *s = TextGetLine( txt );
            if( !s )
            {
                free( psz_text );
                return VLC_EGENERIC;
            }

            if( sscanf( s, "[%d:%d:%d]", &h2, &m2, &s2 ) == 3 )
                p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                        (int64_t)m2 * 60*1000 +
                                        (int64_t)s2 * 1000 ) * 1000;
            else
                p_subtitle->i_stop  = -1;
            break;
        }
        free( psz_text );
    }

    /* replace [br] by \n */
    char *p;
    while( ( p = strstr( psz_text, "[br]" ) ) )
    {
        *p++ = '\n';
        memmove( p, &p[3], strlen(&p[3])+1 );
    }

    p_subtitle->psz_text = psz_text;
    return VLC_SUCCESS;
}

static int ParseSubViewer1( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );

    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char *psz_text;

    for( ;; )
    {
        int h1, m1, s1;
        int h2, m2, s2;
        char *s = TextGetLine( txt );

        if( !s )
            return VLC_EGENERIC;

        if( sscanf( s, "[%d:%d:%d]", &h1, &m1, &s1 ) == 3 )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600*1000 +
                                    (int64_t)m1 * 60*1000 +
                                    (int64_t)s1 * 1000 ) * 1000;

            char *s = TextGetLine( txt );
            if( !s )
                return VLC_EGENERIC;

            psz_text = strdup( s );
            if( !psz_text )
                return VLC_ENOMEM;

            s = TextGetLine( txt );
            if( !s )
            {
                free( psz_text );
                return VLC_EGENERIC;
            }

            if( sscanf( s, "[%d:%d:%d]", &h2, &m2, &s2 ) == 3 )
                p_subtitle->i_stop  = ( (int64_t)h2 * 3600*1000 +
                                        (int64_t)m2 * 60*1000 +
                                        (int64_t)s2 * 1000 ) * 1000;
            else
                p_subtitle->i_stop  = -1;

            break;
        }
    }

    p_subtitle->psz_text = psz_text;

    return VLC_SUCCESS;
}

/* Common code for VTT/SBV since they just differ in timestamps */
static int ParseCommonVTTSBV( demux_t *p_demux, subtitle_t *p_subtitle, int i_idx )
{
    VLC_UNUSED( i_idx );
    demux_sys_t *p_sys = p_demux->p_sys;
    text_t      *txt = &p_sys->txt;
    char        *psz_text;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int h1 = 0, m1 = 0, s1 = 0, d1 = 0;
        int h2 = 0, m2 = 0, s2 = 0, d2 = 0;

        if( !s )
            return VLC_EGENERIC;

        bool b_matched = false;

        if( p_sys->i_type == SUB_TYPE_VTT )
        {
            b_matched =
            ( sscanf( s,"%d:%d.%d --> %d:%d.%d",
                             &m1, &s1, &d1,
                             &m2, &s2, &d2 ) == 6 ||
                sscanf( s,"%d:%d.%d --> %d:%d:%d.%d",
                             &m1, &s1, &d1,
                        &h2, &m2, &s2, &d2 ) == 7 ||
                sscanf( s,"%d:%d:%d.%d --> %d:%d.%d",
                        &h1, &m1, &s1, &d1,
                             &m2, &s2, &d2 ) == 7 ||
                sscanf( s,"%d:%d:%d.%d --> %d:%d:%d.%d",
                        &h1, &m1, &s1, &d1,
                        &h2, &m2, &s2, &d2 ) == 8 );
        }
        else if( p_sys->i_type == SUB_TYPE_SBV )
        {
            b_matched =
            ( sscanf( s,"%d:%d:%d.%d,%d:%d:%d.%d",
                        &h1, &m1, &s1, &d1,
                        &h2, &m2, &s2, &d2 ) == 8 );
        }

        if( b_matched )
        {
            p_subtitle->i_start = ( (int64_t)h1 * 3600 * 1000 +
                                    (int64_t)m1 * 60 * 1000 +
                                    (int64_t)s1 * 1000 +
                                    (int64_t)d1 ) * 1000;

            p_subtitle->i_stop  = ( (int64_t)h2 * 3600 * 1000 +
                                    (int64_t)m2 * 60 * 1000 +
                                    (int64_t)s2 * 1000 +
                                    (int64_t)d2 ) * 1000;
            if( p_subtitle->i_start < p_subtitle->i_stop )
                break;
        }
    }

    /* Now read text until an empty line */
    psz_text = strdup("");
    if( !psz_text )
        return VLC_ENOMEM;

    for( ;; )
    {
        const char *s = TextGetLine( txt );
        int i_len;
        int i_old;

        i_len = s ? strlen( s ) : 0;
        if( i_len <= 0 )
        {
            p_subtitle->psz_text = psz_text;
            return VLC_SUCCESS;
        }

        i_old = strlen( psz_text );
        psz_text = realloc_or_free( psz_text, i_old + i_len + 1 + 1 );
        if( !psz_text )
            return VLC_ENOMEM;

        strcat( psz_text, s );
        strcat( psz_text, "\n" );
    }
}
