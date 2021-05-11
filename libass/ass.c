/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>

#ifdef CONFIG_ICONV
#include <iconv.h>
#endif

#include "ass.h"
#include "ass_utils.h"
#include "ass_library.h"
#include "ass_priv.h"
#include "ass_shaper.h"
#include "ass_string.h"

#define ass_atof(STR) (ass_strtod((STR),NULL))

static const ASS_FormatToken ass_style_format[] = {
    ASS_FMT_Name, ASS_FMT_FontName, ASS_FMT_FontSize, ASS_FMT_PrimaryColour,
    ASS_FMT_SecondaryColour, ASS_FMT_OutlineColour, ASS_FMT_BackColour,
    ASS_FMT_Bold, ASS_FMT_Italic, ASS_FMT_Underline, ASS_FMT_StrikeOut,
    ASS_FMT_ScaleX, ASS_FMT_ScaleY, ASS_FMT_Spacing, ASS_FMT_Angle,
    ASS_FMT_BorderStyle, ASS_FMT_Outline, ASS_FMT_Shadow, ASS_FMT_Alignment,
    ASS_FMT_MarginL, ASS_FMT_MarginR, ASS_FMT_MarginV, ASS_FMT_Encoding
};
static const size_t ass_style_format_length = sizeof(ass_style_format) / sizeof(ass_style_format[0]);

static const ASS_FormatToken ass_event_format[] = {
    ASS_FMT_Layer, ASS_FMT_Start, ASS_FMT_End, ASS_FMT_Style, ASS_FMT_Name,
    ASS_FMT_MarginL, ASS_FMT_MarginR, ASS_FMT_MarginV, ASS_FMT_Effect, ASS_FMT_Text
};
static const size_t ass_event_format_length = sizeof(ass_event_format) / sizeof(ass_event_format[0]);

static const ASS_FormatToken ssa_style_format[] = {
    ASS_FMT_Name, ASS_FMT_FontName, ASS_FMT_FontSize, ASS_FMT_PrimaryColour,
    ASS_FMT_SecondaryColour, ASS_FMT_TertiaryColour, ASS_FMT_BackColour,
    ASS_FMT_Bold, ASS_FMT_Italic, ASS_FMT_BorderStyle, ASS_FMT_Outline,
    ASS_FMT_Shadow, ASS_FMT_Alignment, ASS_FMT_MarginL, ASS_FMT_MarginR,
    ASS_FMT_MarginV, ASS_FMT_AlphaLevel, ASS_FMT_Encoding
};
static const size_t ssa_style_format_length = sizeof(ssa_style_format) / sizeof(ssa_style_format[0]);

static const ASS_FormatToken ssa_event_format[] = {
    ASS_FMT_Marked, ASS_FMT_Start, ASS_FMT_End, ASS_FMT_Style, ASS_FMT_Name,
    ASS_FMT_MarginL, ASS_FMT_MarginR, ASS_FMT_MarginV, ASS_FMT_Effect, ASS_FMT_Text
};
static const size_t ssa_event_format_length = sizeof(ssa_event_format) / sizeof(ssa_event_format[0]);

/* Choose ASS_FORMAT_ALLOC large enough to fit in all tokens once,
   but without risking overflows */
#define ASS_FORMAT_ALLOC 36
#define ASS_STYLES_ALLOC 20


int ass_library_version(void)
{
    return LIBASS_VERSION;
}

void ass_free_track(ASS_Track *track)
{
    int i;

    if (!track)
        return;

    if (track->parser_priv) {
        free(track->parser_priv->read_order_bitmap);
        free(track->parser_priv->fontname);
        free(track->parser_priv->fontdata);
        free(track->parser_priv->style_format.tokens);
        free(track->parser_priv->event_format.tokens);
        free(track->parser_priv->style_format.string);
        free(track->parser_priv->event_format.string);
        free(track->parser_priv);
    }
    free(track->style_format);
    free(track->event_format);
    free(track->Language);
    if (track->styles) {
        for (i = 0; i < track->n_styles; ++i)
            ass_free_style(track, i);
    }
    free(track->styles);
    if (track->events) {
        for (i = 0; i < track->n_events; ++i)
            ass_free_event(track, i);
    }
    free(track->events);
    free(track->name);
    free(track);
}

/// \brief Allocate a new style struct
/// \param track track
/// \return style id or negative value on failure
int ass_alloc_style(ASS_Track *track)
{
    int sid;

    assert(track->n_styles <= track->max_styles);

    if (track->n_styles == track->max_styles) {
        if (track->max_styles >= FFMIN(SIZE_MAX, INT_MAX) - ASS_STYLES_ALLOC)
            return -1;
        int new_max = track->max_styles + ASS_STYLES_ALLOC;
        if (!ASS_REALLOC_ARRAY(track->styles, new_max))
            return -1;
        track->max_styles = new_max;
    }

    sid = track->n_styles++;
    memset(track->styles + sid, 0, sizeof(ASS_Style));
    return sid;
}

/// \brief Allocate a new event struct
/// \param track track
/// \return event id or negative value on failure
int ass_alloc_event(ASS_Track *track)
{
    int eid;

    assert(track->n_events <= track->max_events);

    if (track->n_events == track->max_events) {
        if (track->max_events >= FFMIN(SIZE_MAX, INT_MAX) / 2)
            return -1;
        int new_max = track->max_events * 2 + 1;
        if (!ASS_REALLOC_ARRAY(track->events, new_max))
            return -1;
        track->max_events = new_max;
    }

    eid = track->n_events++;
    memset(track->events + eid, 0, sizeof(ASS_Event));
    return eid;
}

void ass_free_event(ASS_Track *track, int eid)
{
    ASS_Event *event = track->events + eid;

    free(event->Name);
    free(event->Effect);
    free(event->Text);
    free(event->render_priv);
}

void ass_free_style(ASS_Track *track, int sid)
{
    ASS_Style *style = track->styles + sid;

    free(style->Name);
    free(style->FontName);
}

static int resize_read_order_bitmap(ASS_Track *track, int max_id)
{
    // Don't allow malicious files to OOM us easily. Also avoids int overflows.
    if (max_id < 0 || max_id >= 10 * 1024 * 1024 * 8)
        goto fail;
    assert(track->parser_priv->read_order_bitmap || !track->parser_priv->read_order_elems);
    if (max_id >= track->parser_priv->read_order_elems * 32) {
        int oldelems = track->parser_priv->read_order_elems;
        int elems = ((max_id + 31) / 32 + 1) * 2;
        assert(elems >= oldelems);
        track->parser_priv->read_order_elems = elems;
        void *new_bitmap =
            realloc(track->parser_priv->read_order_bitmap, elems * 4);
        if (!new_bitmap)
            goto fail;
        track->parser_priv->read_order_bitmap = new_bitmap;
        memset(track->parser_priv->read_order_bitmap + oldelems, 0,
               (elems - oldelems) * 4);
    }
    return 0;

fail:
    free(track->parser_priv->read_order_bitmap);
    track->parser_priv->read_order_bitmap = NULL;
    track->parser_priv->read_order_elems = 0;
    return -1;
}

static int test_and_set_read_order_bit(ASS_Track *track, int id)
{
    if (resize_read_order_bitmap(track, id) < 0)
        return -1;
    int index = id / 32;
    uint32_t bit = 1u << (id % 32);
    if (track->parser_priv->read_order_bitmap[index] & bit)
        return 1;
    track->parser_priv->read_order_bitmap[index] |= bit;
    return 0;
}

// ==============================================================================================

/**
 * \brief Set up default style
 * \param style style to edit to defaults
 * The parameters are mostly taken directly from VSFilter source for
 * best compatibility.
 */
static void set_default_style(ASS_Style *style)
{
    style->Name             = strdup("Default");
    style->FontName         = strdup("Arial");
    style->FontSize         = 18;
    style->PrimaryColour    = 0xffffff00;
    style->SecondaryColour  = 0x00ffff00;
    style->OutlineColour    = 0x00000000;
    style->BackColour       = 0x00000080;
    style->Bold             = 200;
    style->ScaleX           = 1.0;
    style->ScaleY           = 1.0;
    style->Spacing          = 0;
    style->BorderStyle      = 1;
    style->Outline          = 2;
    style->Shadow           = 3;
    style->Alignment        = 2;
    style->MarginL = style->MarginR = style->MarginV = 20;
}

static long long string2timecode(ASS_Library *library, char *p)
{
    int h, m, s, ms;
    long long tm;
    int res = sscanf(p, "%d:%d:%d.%d", &h, &m, &s, &ms);
    if (res < 4) {
        ass_msg(library, MSGL_WARN, "Bad timestamp");
        return 0;
    }
    tm = ((h * 60LL + m) * 60 + s) * 1000 + ms * 10LL;
    return tm;
}

#define NEXT(str,token) \
    token = next_token(&str); \
    if (!token) break;


#define PARSE_START switch (fmt->tokens[tokenpos]) {
#define PARSE_END   }

#define STRVAL(name) \
    case ASS_FMT_ ## name : \
        free(target->name); \
        target->name = strdup(token); \
        break;

#define STARREDSTRVAL(name) \
    case ASS_FMT_ ## name : \
        while (*token == '*') ++token; \
        free(target->name); \
        target->name = strdup(token); \
        break;

#define TIMEVAL_ALTNAME(fieldname,formatname) \
    case ASS_FMT_ ## formatname : \
        target->fieldname = string2timecode(track->library, token); \
        break;

#define STYLEVAL(name) \
    case ASS_FMT_ ## name : \
        target->name = lookup_style(track, token); \
        break;

#define ANYVAL(name,func) \
    case ASS_FMT_ ## name : \
        target->name = func(token); \
        break;

#define COLORVAL(name) ANYVAL(name,parse_color_header)
#define INTVAL(name)   ANYVAL(name,atoi)
#define FPVAL(name)    ANYVAL(name,ass_atof)

// skip spaces in str beforehand, or trim leading spaces afterwards
static inline void advance_token_pos(const char **const str,
                                     const char **const start,
                                     const char **const end)
{
    *start = *str;
    *end   = *start;
    while (**end != '\0' && **end != ',') ++*end;
    *str = *end + (**end == ',');
    rskip_spaces((char**)end, (char*)*start);
}

static char *next_token(char **str)
{
    char *p;
    char *start;
    skip_spaces(str);
    if (**str == '\0') {
        return 0;
    }

    advance_token_pos((const char**)str,
                      (const char**)&start,
                      (const char**)&p);

    *p = '\0';
    return start;
}

static int parse_format_line(ASS_FormatLine *fmt, const char *string)
{
    fmt->n_tokens = 0;

    free(fmt->string);
    fmt->string = strdup(string);
    if (!fmt->string)
        return -1;

    skip_spaces((char **) &string);
    while (*string) {
        // NEXT would mess up track's fmt string on resync, so don't use it.
        const char *tk_start, *tk_end;
        advance_token_pos(&string, &tk_start, &tk_end);
        skip_spaces((char **) &string);

        if (fmt->n_tokens >= fmt->max_tokens) {
            if (fmt->max_tokens >= SIZE_MAX/2 - 1 ||
                    !ASS_REALLOC_ARRAY(fmt->tokens, 2*fmt->max_tokens + 1))
                return -1;
        }

        #define FMTTOKEN(name) \
            else if (tk_end-tk_start == sizeof( #name ) - 1 && \
                    strncasecmp(tk_start, #name, tk_end-tk_start) == 0) { \
                fmt->tokens[fmt->n_tokens++] = ASS_FMT_ ## name ; \
            }

        if (0) {}
        FMTTOKEN(Name)
        FMTTOKEN(FontName)
        FMTTOKEN(FontSize)
        FMTTOKEN(PrimaryColour)
        FMTTOKEN(SecondaryColour)
        FMTTOKEN(OutlineColour)
        FMTTOKEN(BackColour)
        FMTTOKEN(Bold)
        FMTTOKEN(Italic)
        FMTTOKEN(Underline)
        FMTTOKEN(StrikeOut)
        FMTTOKEN(ScaleX)
        FMTTOKEN(ScaleY)
        FMTTOKEN(Spacing)
        FMTTOKEN(Angle)
        FMTTOKEN(BorderStyle)
        FMTTOKEN(Outline)
        FMTTOKEN(Shadow)
        FMTTOKEN(Alignment)
        FMTTOKEN(MarginL)
        FMTTOKEN(MarginR)
        FMTTOKEN(MarginV)
        FMTTOKEN(Encoding)
        FMTTOKEN(Layer)
        FMTTOKEN(Start)
        FMTTOKEN(End)
        FMTTOKEN(Style)
        FMTTOKEN(Effect)
        FMTTOKEN(Text)
        FMTTOKEN(TertiaryColour)
        FMTTOKEN(AlphaLevel)
        FMTTOKEN(Marked)
        FMTTOKEN(RelativeTo)
        FMTTOKEN(MarginT)
        FMTTOKEN(MarginB)
        else {
            fmt->tokens[fmt->n_tokens++] = ASS_FMT_UNKNOWN;
        }

        #undef FMTTOKEN
    }

    return 0;
}

/**
 * \brief Parse the tail of Dialogue line
 * \param track track
 * \param event parsed data goes here
 * \param str string to parse, zero-terminated
 * \param n_ignored number of format options to skip at the beginning
*/
static int process_event_tail(ASS_Track *track, ASS_Event *event,
                              char *str, int n_ignored)
{
    char *token;
    char *p = str;
    ASS_Event *target = event;
    size_t tokenpos = 0;
    ASS_FormatLine *fmt = &track->parser_priv->event_format;

    for (tokenpos = n_ignored; tokenpos + 1 < fmt->n_tokens; ++tokenpos) {
        NEXT(p, token);

        PARSE_START
            INTVAL(Layer)
            STYLEVAL(Style)
            STRVAL(Name)
            STRVAL(Effect)
            INTVAL(MarginL)
            INTVAL(MarginR)
            INTVAL(MarginV)
            TIMEVAL_ALTNAME(Start,Start)
            TIMEVAL_ALTNAME(Duration,End)
            default:
                //Nothing
                break;
        PARSE_END
    }
    if (tokenpos != fmt->n_tokens - 1)
        return 1;
    // "Text" is always last and may contain commas
    event->Text = strdup(p);
    if (event->Text && *event->Text != 0) {
        char *last = event->Text + strlen(event->Text) - 1;
        if (last >= event->Text && *last == '\r')
            *last = 0;
    }
    event->Duration -= event->Start;
    return event->Text ? 0 : -1;
}


/* One section started with PARSE_START and PARSE_END parses a single token
 * (contained in the variable named token) for the header indicated by the
 * variable tname. It does so by chaining a number of else-if statements, each
 * of which checks if the tname variable indicates that this header should be
 * parsed. The first parameter of the macro gives the name of the header.
 *
 * The string that is passed is in str. str is advanced to the next token if
 * a header could be parsed. The parsed results are stored in the variable
 * target, which has the type ASS_Style* (or ASS_Event*).
 */
#define PARSE_START_STR if (0) {
#define PARSE_END_STR   }

#define ANYVAL_STR(name,func) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        target->name = func(token);

#define STRVAL_STR(name) \
    } else if (ass_strcasecmp(tname, #name) == 0) { \
        char *new_str = strdup(token); \
        if (new_str) { \
            free(target->name); \
            target->name = new_str; \
        }

#define COLORVAL_STR(name) ANYVAL_STR(name,parse_color_header)
#define INTVAL_STR(name) ANYVAL_STR(name,atoi)
#define FPVAL_STR(name) ANYVAL_STR(name,ass_atof)


/**
 * \brief Parse command line style overrides (--ass-force-style option)
 * \param track track to apply overrides to
 * The format for overrides is [StyleName.]Field=Value
 */
void ass_process_force_style(ASS_Track *track)
{
    char **fs, *eq, *dt, *style, *tname, *token;
    ASS_Style *target;
    int sid;
    char **list = track->library->style_overrides;

    if (!list)
        return;

    for (fs = list; *fs; ++fs) {
        eq = strrchr(*fs, '=');
        if (!eq)
            continue;
        *eq = '\0';
        token = eq + 1;

        if (!ass_strcasecmp(*fs, "PlayResX"))
            track->PlayResX = atoi(token);
        else if (!ass_strcasecmp(*fs, "PlayResY"))
            track->PlayResY = atoi(token);
        else if (!ass_strcasecmp(*fs, "Timer"))
            track->Timer = ass_atof(token);
        else if (!ass_strcasecmp(*fs, "WrapStyle"))
            track->WrapStyle = atoi(token);
        else if (!ass_strcasecmp(*fs, "ScaledBorderAndShadow"))
            track->ScaledBorderAndShadow = parse_bool(token);
        else if (!ass_strcasecmp(*fs, "Kerning"))
            track->Kerning = parse_bool(token);
        else if (!ass_strcasecmp(*fs, "YCbCr Matrix"))
            track->YCbCrMatrix = parse_ycbcr_matrix(token);

        dt = strrchr(*fs, '.');
        if (dt) {
            *dt = '\0';
            style = *fs;
            tname = dt + 1;
        } else {
            style = NULL;
            tname = *fs;
        }
        for (sid = 0; sid < track->n_styles; ++sid) {
            if (style == NULL
                || ass_strcasecmp(track->styles[sid].Name, style) == 0) {
                target = track->styles + sid;
                PARSE_START_STR
                    STRVAL_STR(FontName)
                    COLORVAL_STR(PrimaryColour)
                    COLORVAL_STR(SecondaryColour)
                    COLORVAL_STR(OutlineColour)
                    COLORVAL_STR(BackColour)
                    FPVAL_STR(FontSize)
                    INTVAL_STR(Bold)
                    INTVAL_STR(Italic)
                    INTVAL_STR(Underline)
                    INTVAL_STR(StrikeOut)
                    FPVAL_STR(Spacing)
                    FPVAL_STR(Angle)
                    INTVAL_STR(BorderStyle)
                    INTVAL_STR(Alignment)
                    INTVAL_STR(Justify)
                    INTVAL_STR(MarginL)
                    INTVAL_STR(MarginR)
                    INTVAL_STR(MarginV)
                    INTVAL_STR(Encoding)
                    FPVAL_STR(ScaleX)
                    FPVAL_STR(ScaleY)
                    FPVAL_STR(Outline)
                    FPVAL_STR(Shadow)
                    FPVAL_STR(Blur)
                PARSE_END_STR
            }
        }
        *eq = '=';
        if (dt)
            *dt = '.';
    }
}

#undef PARSE_START_STR
#undef PARSE_END_STR
#undef STRVAL_STR
#undef COLORVAL_STR
#undef FPVAL_STR
#undef INTVAL_STR

/**
 * \brief Parse the Style line
 * \param track track
 * \param str string to parse, zero-terminated
 * Allocates a new style struct.
*/
static int process_style(ASS_Track *track, char *str)
{

    char *token;
    char *p = str;
    int sid;
    ASS_Style *style;
    ASS_Style *target;
    size_t tokenpos;
    ASS_FormatLine *fmt = &track->parser_priv->style_format;

    if (!fmt->n_tokens) {
        // no style format header
        // probably an ancient script version
        if (track->track_type == TRACK_TYPE_SSA) {
            fmt->n_tokens = ssa_style_format_length;
            memcpy(fmt->tokens, ssa_style_format, sizeof(ssa_style_format));
        } else {
            fmt->n_tokens = ass_style_format_length;
            memcpy(fmt->tokens, ass_style_format, sizeof(ass_style_format));
        }
    }

    ass_msg(track->library, MSGL_V, "[%p] Style: %s", track, str);

    sid = ass_alloc_style(track);
    if (sid < 0)
        return -1;

    style = track->styles + sid;
    target = style;

    // fill style with some default values
    style->ScaleX = 100.;
    style->ScaleY = 100.;

    for (tokenpos = 0; tokenpos < fmt->n_tokens; ++tokenpos) {
        NEXT(p, token); //breaks if no more tokens

        PARSE_START
            STARREDSTRVAL(Name)
            STRVAL(FontName)
            COLORVAL(PrimaryColour)
            COLORVAL(SecondaryColour)
            COLORVAL(OutlineColour) // TertiaryColor
            COLORVAL(BackColour)
            FPVAL(FontSize)
            INTVAL(Bold)
            INTVAL(Italic)
            INTVAL(Underline)
            INTVAL(StrikeOut)
            FPVAL(Spacing)
            FPVAL(Angle)
            INTVAL(BorderStyle)
            INTVAL(Alignment)
            INTVAL(MarginL)
            INTVAL(MarginR)
            INTVAL(MarginV)
            INTVAL(Encoding)
            FPVAL(ScaleX)
            FPVAL(ScaleY)
            FPVAL(Outline)
            FPVAL(Shadow)
            default:
                //Nothing
                break;
        PARSE_END
    }
    if (tokenpos != fmt->n_tokens) {
        ass_free_style(track, sid);
        track->n_styles--;
        return 1;
    }

    // Compat adjustments
    if (track->track_type == TRACK_TYPE_SSA) {
        // SSA uses BackColour for both outline and shadow
        // this will destroy SSA's TertiaryColour, but i'm not going to use it anyway
        target->OutlineColour = target->BackColour;

        // VSFilter compat
        if (target->Alignment == 8)
            target->Alignment = 3;
        else if (target->Alignment == 4)
            target->Alignment = 11;
    } else {
        // Convert ASS alignment to SSA
        target->Alignment = numpad2align(target->Alignment);
    }

    // Sanitise values
    style->ScaleX = FFMAX(style->ScaleX, 0.) / 100.;
    style->ScaleY = FFMAX(style->ScaleY, 0.) / 100.;
    style->Spacing = FFMAX(style->Spacing, 0.);
    style->Outline = FFMAX(style->Outline, 0.);
    style->Shadow = FFMAX(style->Shadow, 0.);
    style->Bold = !!style->Bold;
    style->Italic = !!style->Italic;
    style->Underline = !!style->Underline;
    style->StrikeOut = !!style->StrikeOut;
    if (!style->Name)
        style->Name = strdup("Default");
    if (!style->FontName)
        style->FontName = strdup("Arial");
    if (!style->Name || !style->FontName) {
        ass_free_style(track, sid);
        track->n_styles--;
        return -1;
    }
    if (strcmp(target->Name, "Default") == 0)
        track->default_style = sid;
    return 0;
}

static bool format_line_compare(const ASS_FormatToken *fmt1, size_t num1,
                                const ASS_FormatToken *fmt2, size_t num2)
{
    if (num1 != num2)
        return false;
    for (size_t i = 0; i < num1; ++i) {
        if (*fmt1 != *fmt2)
            return false;
        ++fmt1;
        ++fmt2;
    }
    return true;
}


/**
 * \brief Set SBAS=1 if not set explicitly in case of custom format line
 * \param track   track
 * \param fmt     format line of file
 * \param num_fmt number of format token in fmt
 * \param std     standard format line
 * \param num_std number of format token in std
 *
 * As of writing libass is the only renderer accepting custom format lines.
 * For years libass defaultet SBAS to yes instead of no.
 * To avoid breaking released scripts with custom format lines,
 * keep SBAS=1 default for custom format files.
 */
static void custom_format_line_compatibility(ASS_Track *const track,
                                             const ASS_FormatToken *fmt,
                                             size_t num_fmt,
                                             const ASS_FormatToken *std,
                                             size_t num_std)
{
    if (!(track->parser_priv->header_flags & SINFO_SCALEDBORDER)
        && !format_line_compare(fmt, num_fmt, std, num_std)) {
        ass_msg(track->library, MSGL_INFO,
               "Track has custom format line(s). "
                "'ScaledBorderAndShadow' will default to 'yes'.");
        track->ScaledBorderAndShadow = 1;
    }
}

static int process_styles_line(ASS_Track *track, char *str)
{
    int ret = 0;
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->style_format);
        track->style_format = strdup(p);
        ass_msg(track->library, MSGL_DBG2, "Style format: %s", p);
        if (parse_format_line(&track->parser_priv->style_format, p))
            return -1;
        if (track->track_type == TRACK_TYPE_ASS) {
            custom_format_line_compatibility(track,
                    track->parser_priv->style_format.tokens,
                    track->parser_priv->style_format.n_tokens,
                    ass_style_format, ass_style_format_length);
        } else {
            custom_format_line_compatibility(track,
                    track->parser_priv->style_format.tokens,
                    track->parser_priv->style_format.n_tokens,
                    ssa_style_format, ssa_style_format_length);
        }
    } else if (!strncmp(str, "Style:", 6)) {
        char *p = str + 6;
        skip_spaces(&p);
        ret = process_style(track, p);
    }
    return ret;
}

static inline void check_duplicate_info_line(const ASS_Track *const track,
                                             const ScriptInfo si,
                                             const char *const name)
{
    if (track->parser_priv->header_flags & si)
        ass_msg(track->library, MSGL_WARN,
            "Duplicate Script Info Header '%s'. Previous value overwritten!",
            name);
    else
        track->parser_priv->header_flags |= si;
}

static int process_info_line(ASS_Track *track, char *str)
{
    if (!strncmp(str, "PlayResX:", 9)) {
        check_duplicate_info_line(track, SINFO_PLAYRESX, "PlayResX");
        track->PlayResX = atoi(str + 9);
    } else if (!strncmp(str, "PlayResY:", 9)) {
        check_duplicate_info_line(track, SINFO_PLAYRESY, "PlayResY");
        track->PlayResY = atoi(str + 9);
    } else if (!strncmp(str, "Timer:", 6)) {
        check_duplicate_info_line(track, SINFO_TIMER, "Timer");
        track->Timer = ass_atof(str + 6);
    } else if (!strncmp(str, "WrapStyle:", 10)) {
        check_duplicate_info_line(track, SINFO_WRAPSTYLE, "WrapStyle");
        track->WrapStyle = atoi(str + 10);
    } else if (!strncmp(str, "ScaledBorderAndShadow:", 22)) {
        check_duplicate_info_line(track, SINFO_SCALEDBORDER,
                                    "ScaledBorderAndShadow");
        track->ScaledBorderAndShadow = parse_bool(str + 22);
    } else if (!strncmp(str, "Kerning:", 8)) {
        check_duplicate_info_line(track, SINFO_KERNING, "Kerning");
        track->Kerning = parse_bool(str + 8);
    } else if (!strncmp(str, "YCbCr Matrix:", 13)) {
        check_duplicate_info_line(track, SINFO_COLOURMATRIX, "YCbCr Matrix");
        track->YCbCrMatrix = parse_ycbcr_matrix(str + 13);
    } else if (!strncmp(str, "Language:", 9)) {
        check_duplicate_info_line(track, SINFO_LANGUAGE, "Language");
        char *p = str + 9;
        while (*p && ass_isspace(*p)) p++;
        free(track->Language);
        track->Language = strndup(p, 2);
    } else if (!strncmp(str, "; Script generated by ", 22)) {
        if (!strncmp(str + 22,"FFmpeg/Lavc", 11))
            track->parser_priv->header_flags |= GENBY_FFMPEG;
    }
    return 0;
}

static void event_format_fallback(ASS_Track *track)
{
    ASS_ParserPriv* const pp = track->parser_priv;
    pp->state = PST_EVENTS;
    if (track->track_type == TRACK_TYPE_SSA) {
        pp->event_format.n_tokens = ssa_event_format_length;
        memcpy(pp->event_format.tokens, ssa_event_format, sizeof(ssa_event_format));
    } else {
        pp->event_format.n_tokens = ass_event_format_length;
        memcpy(pp->event_format.tokens, ass_event_format, sizeof(ass_event_format));
    }
    ass_msg(track->library, MSGL_V,
            "No event format found, using fallback");
}

/**
 * \brief Return if track is post-signature and pre-SBAS ffmpeg track
 * \param track track
*/
static bool detect_legacy_conv_subs(ASS_Track *track)
{
    /*
     * FFmpeg and libav convert srt subtitles to ass.
     * In legacy versions, they did not set the 'ScaledBorderAndShadow' header,
     * but expected it to default to yes (which libass did).
     * To avoid breaking them, we try to detect these
     * converted subs by common properties of ffmpeg/libav's converted subs.
     * Since files with custom format lines (-2014.10.11) default to SBAS=1
     * regardless of being ffmpeg generated or not, we are only concerned with
     * post-signature and pre-SBAS ffmpeg-files (2014.10.11-2020.04.17).
     * We want to avoid matching modified ffmpeg files though.
     *
     * Relevant ffmpeg commits are:
     *  2c77c90684e24ef16f7e7c4462e011434cee6a98  2010.12.29
     *    Initial conversion format.
     *    Style "Format:" line is mix of SSA and ASS
     *    Event "Format:" line
     *     "Format: Layer, Start, End, Text\r\n"
     *    Only Header in ScriptInfo is "ScriptType: v4.00+"
     *  0e7782c08ec77739edb0b98ba5d896b45e98235f  2012.06.15
     *    Adds 'Style' to Event "Format:" line
     *  5039aadf68deb9ad6dd0737ea11259fe53d3727b  2014.06.18
     *    Adds PlayerRes(X|Y) (384x288)
     *    (moved below ScriptType: a few minutes later)
     *  40b9f28641b696c6bb73ce49dc97c2ce2700cbdb  2014.10.11 14:31:23 +0200
     *    Regular full ASS Event and Style "Format:" lines
     *  52b0a0ecaa02e17f7e01bead8c3f215f1cfd48dc  2014.10.11 18:37:43 +0200 <==
     *    Signature comment
     *  56bc0a6736cdc7edab837ff8f304661fd16de0e4  2015.02.08
     *    Allow custom PlayRes(X|Y)
     *  a8ba2a2c1294a330a0e79ae7f0d3a203a7599166  2020.04.17
     *    Set 'ScaledBorderAndShadow: yes'
     *
     * libav outputs initial ffmpeg format. (no longer maintained)
     */

    // GENBY_FFMPEG and exact ffmpeg headers required
    // Note: If there's SINFO_SCRIPTTYPE in the future this needs to be updated
    if (track->parser_priv->header_flags
            ^ (SINFO_PLAYRESX | SINFO_PLAYRESY | GENBY_FFMPEG))
        return false;

    // Legacy ffmpeg only ever has one style
    // Check 2 not 1 because libass also adds a def style
    if (track->n_styles != 2
        || strncmp(track->styles[1].Name, "Default", 7))
        return false;

    return true;
}


static int process_events_line(ASS_Track *track, char *str)
{
    if (!strncmp(str, "Format:", 7)) {
        char *p = str + 7;
        skip_spaces(&p);
        free(track->event_format);
        track->event_format = strdup(p);
        ass_msg(track->library, MSGL_DBG2, "Event format: %s", p);
        if (parse_format_line(&track->parser_priv->event_format, p))
            return -1;
        if (track->parser_priv->event_format.n_tokens > 0 &&
                track->parser_priv->event_format.tokens[track->parser_priv->event_format.n_tokens-1] != ASS_FMT_Text) {
            ass_msg(track->library, MSGL_WARN, "Event Format line does not end with Text! Pretend it does.");
        }
        if (track->track_type == TRACK_TYPE_ASS) {
            custom_format_line_compatibility(track,
                    track->parser_priv->event_format.tokens,
                    track->parser_priv->event_format.n_tokens,
                    ass_event_format, ass_event_format_length);
        } else {
            custom_format_line_compatibility(track,
                    track->parser_priv->event_format.tokens,
                    track->parser_priv->event_format.n_tokens,
                    ssa_event_format, ssa_event_format_length);
        }

        // Guess if we are dealing with legacy ffmpeg subs and change accordingly
        // If file has no event format it was probably not created by ffmpeg/libav
        if (detect_legacy_conv_subs(track)) {
            track->ScaledBorderAndShadow = 1;
            ass_msg(track->library, MSGL_INFO,
                    "Track treated as legacy ffmpeg sub.");
        }
    } else if (!strncmp(str, "Dialogue:", 9)) {
        // This should never be reached for embedded subtitles.
        // They have slightly different format and are parsed in ass_process_chunk,
        // called directly from demuxer
        int eid;
        ASS_Event *event;

        // We can't parse events without event_format
        if (!track->parser_priv->event_format.n_tokens) {
            event_format_fallback(track);
        }

        str += 9;
        skip_spaces(&str);

        eid = ass_alloc_event(track);
        if (eid < 0)
            return -1;
        event = track->events + eid;

        return process_event_tail(track, event, str, 0);
    } else {
        ass_msg(track->library, MSGL_V, "Not understood: '%.30s'", str);
    }
    return 0;
}

static unsigned char *decode_chars(const unsigned char *src,
                                   unsigned char *dst, size_t cnt_in)
{
    uint32_t value = 0;
    for (size_t i = 0; i < cnt_in; i++)
        value |= (uint32_t) ((src[i] - 33u) & 63) << 6 * (3 - i);

    *dst++ = value >> 16;
    if (cnt_in >= 3)
        *dst++ = value >> 8 & 0xff;
    if (cnt_in >= 4)
        *dst++ = value & 0xff;
    return dst;
}

static void reset_embedded_font_parsing(ASS_ParserPriv *parser_priv)
{
    free(parser_priv->fontname);
    free(parser_priv->fontdata);
    parser_priv->fontname = NULL;
    parser_priv->fontdata = NULL;
    parser_priv->fontdata_size = 0;
    parser_priv->fontdata_used = 0;
}

static int decode_font(ASS_Track *track)
{
    unsigned char *p;
    unsigned char *q;
    size_t i;
    size_t size;                   // original size
    size_t dsize;                  // decoded size
    unsigned char *buf = 0;

    ass_msg(track->library, MSGL_V, "Font: %zu bytes encoded data",
            track->parser_priv->fontdata_used);
    size = track->parser_priv->fontdata_used;
    if (size % 4 == 1) {
        ass_msg(track->library, MSGL_ERR, "Bad encoded data size");
        goto error_decode_font;
    }
    buf = malloc(size / 4 * 3 + FFMAX(size % 4, 1) - 1);
    if (!buf)
        goto error_decode_font;
    q = buf;
    for (i = 0, p = (unsigned char *) track->parser_priv->fontdata;
         i < size / 4; i++, p += 4) {
        q = decode_chars(p, q, 4);
    }
    if (size % 4 == 2) {
        q = decode_chars(p, q, 2);
    } else if (size % 4 == 3) {
        q = decode_chars(p, q, 3);
    }
    dsize = q - buf;
    assert(dsize == size / 4 * 3 + FFMAX(size % 4, 1) - 1);

    if (track->library->extract_fonts) {
        ass_add_font(track->library, track->parser_priv->fontname,
                     (char *) buf, dsize);
    }

error_decode_font:
    free(buf);
    reset_embedded_font_parsing(track->parser_priv);
    return 0;
}

static int process_fonts_line(ASS_Track *track, char *str)
{
    size_t len;

    if (!strncmp(str, "fontname:", 9)) {
        char *p = str + 9;
        skip_spaces(&p);
        if (track->parser_priv->fontname) {
            decode_font(track);
        }
        track->parser_priv->fontname = strdup(p);
        if (!track->parser_priv->fontname)
            return -1;
        ass_msg(track->library, MSGL_V, "Fontname: %s",
                track->parser_priv->fontname);
        return 0;
    }

    if (!track->parser_priv->fontname) {
        ass_msg(track->library, MSGL_V, "Not understood: '%s'", str);
        return 1;
    }

    len = strlen(str);
    if (track->parser_priv->fontdata_used >=
        SIZE_MAX - FFMAX(len, 100 * 1024)) {
        goto mem_fail;
    } else if (track->parser_priv->fontdata_used + len >
               track->parser_priv->fontdata_size) {
        size_t new_size =
                track->parser_priv->fontdata_size + FFMAX(len, 100 * 1024);
        if (!ASS_REALLOC_ARRAY(track->parser_priv->fontdata, new_size))
            goto mem_fail;
        track->parser_priv->fontdata_size = new_size;
    }
    memcpy(track->parser_priv->fontdata + track->parser_priv->fontdata_used,
           str, len);
    track->parser_priv->fontdata_used += len;

    return 0;

mem_fail:
    reset_embedded_font_parsing(track->parser_priv);
    return -1;
}

/**
 * \brief Parse a header line
 * \param track track
 * \param str string to parse, zero-terminated
*/
static int process_line(ASS_Track *track, char *str)
{
    skip_spaces(&str);
    if (!ass_strncasecmp(str, "[Script Info]", 13)) {
        track->parser_priv->state = PST_INFO;
    } else if (!ass_strncasecmp(str, "[V4 Styles]", 11)) {
        track->parser_priv->state = PST_STYLES;
        track->track_type = TRACK_TYPE_SSA;
    } else if (!ass_strncasecmp(str, "[V4+ Styles]", 12)) {
        track->parser_priv->state = PST_STYLES;
        track->track_type = TRACK_TYPE_ASS;
    } else if (!ass_strncasecmp(str, "[Events]", 8)) {
        track->parser_priv->state = PST_EVENTS;
    } else if (!ass_strncasecmp(str, "[Fonts]", 7)) {
        track->parser_priv->state = PST_FONTS;
    } else {
        switch (track->parser_priv->state) {
        case PST_INFO:
            process_info_line(track, str);
            break;
        case PST_STYLES:
            process_styles_line(track, str);
            break;
        case PST_EVENTS:
            process_events_line(track, str);
            break;
        case PST_FONTS:
            process_fonts_line(track, str);
            break;
        default:
            break;
        }
    }
    return 0;
}

// API users can modify format string in track struct
static inline int sync_format_lines(const char *track_fmt, ASS_FormatLine *priv_fmt,
                                    const char *name, ASS_Library *lib)
{
    if (track_fmt && priv_fmt->string
            && strcmp(track_fmt, priv_fmt->string)) {
        ass_msg(lib, MSGL_V, "%s format string changed via track struct", name);
        return parse_format_line(priv_fmt, track_fmt);
    }
    return 0;
}

static int process_text(ASS_Track *track, char *str)
{
    if (sync_format_lines(track->event_format, &track->parser_priv->event_format,
                         "Event", track->library)
            || sync_format_lines(track->style_format, &track->parser_priv->style_format,
                                 "Style", track->library))
        return -1;

    char *p = str;
    while (1) {
        char *q;
        while (1) {
            if ((*p == '\r') || (*p == '\n'))
                ++p;
            else if (p[0] == '\xef' && p[1] == '\xbb' && p[2] == '\xbf')
                p += 3;         // U+FFFE (BOM)
            else
                break;
        }
        for (q = p; ((*q != '\0') && (*q != '\r') && (*q != '\n')); ++q) {
        };
        if (q == p)
            break;
        if (*q != '\0')
            *(q++) = '\0';
        process_line(track, p);
        if (*q == '\0')
            break;
        p = q;
    }
    // there is no explicit end-of-font marker in ssa/ass
    if (track->parser_priv->fontname)
        decode_font(track);
    return 0;
}

/**
 * \brief Process a chunk of subtitle stream data.
 * \param track track
 * \param data string to parse
 * \param size length of data
*/
void ass_process_data(ASS_Track *track, char *data, int size)
{
    char *str = malloc(size + 1);
    if (!str)
        return;

    memcpy(str, data, size);
    str[size] = '\0';

    ass_msg(track->library, MSGL_V, "Event: %s", str);
    process_text(track, str);
    free(str);
}

/**
 * \brief Process CodecPrivate section of subtitle stream
 * \param track track
 * \param data string to parse
 * \param size length of data
 CodecPrivate section contains [Stream Info] and [V4+ Styles] ([V4 Styles] for SSA) sections
*/
void ass_process_codec_private(ASS_Track *track, char *data, int size)
{
    ass_process_data(track, data, size);

    // probably an mkv produced by ancient mkvtoolnix
    // such files don't have [Events] and Format: headers
    if (!track->parser_priv->event_format.n_tokens)
        event_format_fallback(track);

    ass_process_force_style(track);
}

static int check_duplicate_event(ASS_Track *track, int ReadOrder)
{
    if (track->parser_priv->read_order_bitmap)
        return test_and_set_read_order_bit(track, ReadOrder) > 0;
    // ignoring last event, it is the one we are comparing with
    for (int i = 0; i < track->n_events - 1; i++)
        if (track->events[i].ReadOrder == ReadOrder)
            return 1;
    return 0;
}

void ass_set_check_readorder(ASS_Track *track, int check_readorder)
{
    track->parser_priv->check_readorder = check_readorder == 1;
}

/**
 * \brief Process a chunk of subtitle stream data. In Matroska, this contains exactly 1 event (or a commentary).
 * \param track track
 * \param data string to parse
 * \param size length of data
 * \param timecode starting time of the event (milliseconds)
 * \param duration duration of the event (milliseconds)
*/
void ass_process_chunk(ASS_Track *track, char *data, int size,
                       long long timecode, long long duration)
{
    char *str = NULL;
    int eid;
    char *p;
    char *token;
    ASS_Event *event;
    int check_readorder = track->parser_priv->check_readorder;

    if (check_readorder && !track->parser_priv->read_order_bitmap) {
        for (int i = 0; i < track->n_events; i++) {
            if (test_and_set_read_order_bit(track, track->events[i].ReadOrder) < 0)
                break;
        }
    }

    if (sync_format_lines(track->event_format, &track->parser_priv->event_format,
                          "Event", track->library))
        return;

    if (!track->parser_priv->event_format.n_tokens) {
        ass_msg(track->library, MSGL_WARN, "Event format header missing");
        goto cleanup;
    }

    str = malloc(size + 1);
    if (!str)
        goto cleanup;
    memcpy(str, data, size);
    str[size] = '\0';
    ass_msg(track->library, MSGL_V, "Event at %" PRId64 ", +%" PRId64 ": %s",
           (int64_t) timecode, (int64_t) duration, str);

    eid = ass_alloc_event(track);
    if (eid < 0)
        goto cleanup;
    event = track->events + eid;

    p = str;

    do {
        NEXT(p, token);
        event->ReadOrder = atoi(token);
        if (check_readorder && check_duplicate_event(track, event->ReadOrder))
            break;

        NEXT(p, token);
        event->Layer = atoi(token);

        process_event_tail(track, event, p, 3);

        event->Start = timecode;
        event->Duration = duration;

        goto cleanup;
//              dump_events(tid);
    } while (0);
    // some error
    ass_free_event(track, eid);
    track->n_events--;

cleanup:
    free(str);
}

/**
 * \brief Flush buffered events.
 * \param track track
*/
void ass_flush_events(ASS_Track *track)
{
    if (track->events) {
        int eid;
        for (eid = 0; eid < track->n_events; eid++)
            ass_free_event(track, eid);
        track->n_events = 0;
    }
    free(track->parser_priv->read_order_bitmap);
    track->parser_priv->read_order_bitmap = NULL;
    track->parser_priv->read_order_elems = 0;
}

#ifdef CONFIG_ICONV
/** \brief recode buffer to utf-8
 * constraint: codepage != 0
 * \param data pointer to text buffer
 * \param size buffer size
 * \return a pointer to recoded buffer, caller is responsible for freeing it
**/
static char *sub_recode(ASS_Library *library, char *data, size_t size,
                        char *codepage)
{
    iconv_t icdsc;
    char *tocp = "UTF-8";
    char *outbuf;
    assert(codepage);

    if ((icdsc = iconv_open(tocp, codepage)) != (iconv_t) (-1)) {
        ass_msg(library, MSGL_V, "Opened iconv descriptor");
    } else {
        ass_msg(library, MSGL_ERR, "Error opening iconv descriptor");
        return NULL;
    }

    {
        size_t osize = size;
        size_t ileft = size;
        size_t oleft = size - 1;
        char *ip;
        char *op;
        size_t rc;
        int clear = 0;

        outbuf = malloc(osize);
        if (!outbuf)
            goto out;
        ip = data;
        op = outbuf;

        while (1) {
            if (ileft)
                rc = iconv(icdsc, &ip, &ileft, &op, &oleft);
            else {              // clear the conversion state and leave
                clear = 1;
                rc = iconv(icdsc, NULL, NULL, &op, &oleft);
            }
            if (rc == (size_t) (-1)) {
                if (errno == E2BIG) {
                    size_t offset = op - outbuf;
                    char *nbuf = realloc(outbuf, osize + size);
                    if (!nbuf) {
                        free(outbuf);
                        outbuf = 0;
                        goto out;
                    }
                    outbuf = nbuf;
                    op = outbuf + offset;
                    osize += size;
                    oleft += size;
                } else {
                    ass_msg(library, MSGL_WARN, "Error recoding file");
                    free(outbuf);
                    outbuf = NULL;
                    goto out;
                }
            } else if (clear)
                break;
        }
        outbuf[osize - oleft - 1] = 0;
    }

out:
    if (icdsc != (iconv_t) (-1)) {
        (void) iconv_close(icdsc);
        ass_msg(library, MSGL_V, "Closed iconv descriptor");
    }

    return outbuf;
}
#endif                          // ICONV

/**
 * \brief read file contents into newly allocated buffer
 * \param fname file name
 * \param bufsize out: file size
 * \return pointer to file contents. Caller is responsible for its deallocation.
 */
char *read_file(ASS_Library *library, char *fname, size_t *bufsize)
{
    int res;
    long sz;
    long bytes_read;
    char *buf;

    FILE *fp = fopen(fname, "rb");
    if (!fp) {
        ass_msg(library, MSGL_WARN,
                "ass_read_file(%s): fopen failed", fname);
        return 0;
    }
    res = fseek(fp, 0, SEEK_END);
    if (res == -1) {
        ass_msg(library, MSGL_WARN,
                "ass_read_file(%s): fseek failed", fname);
        fclose(fp);
        return 0;
    }

    sz = ftell(fp);
    rewind(fp);

    ass_msg(library, MSGL_V, "File size: %ld", sz);

    buf = sz < SIZE_MAX ? malloc(sz + 1) : NULL;
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    assert(buf);
    bytes_read = 0;
    do {
        res = fread(buf + bytes_read, 1, sz - bytes_read, fp);
        if (res <= 0) {
            ass_msg(library, MSGL_INFO, "Read failed, %d: %s", errno,
                    strerror(errno));
            fclose(fp);
            free(buf);
            return 0;
        }
        bytes_read += res;
    } while (sz - bytes_read > 0);
    buf[sz] = '\0';
    fclose(fp);

    if (bufsize)
        *bufsize = sz;
    return buf;
}

/*
 * \param buf pointer to subtitle text in utf-8
 */
static ASS_Track *parse_memory(ASS_Library *library, char *buf)
{
    ASS_Track *track;
    int i;

    track = ass_new_track(library);
    if (!track)
        return NULL;

    // process header
    process_text(track, buf);

    // external SSA/ASS subs does not have ReadOrder field
    for (i = 0; i < track->n_events; ++i)
        track->events[i].ReadOrder = i;

    if (track->track_type == TRACK_TYPE_UNKNOWN) {
        ass_free_track(track);
        return 0;
    }

    ass_process_force_style(track);

    return track;
}

/**
 * \brief Read subtitles from memory.
 * \param library libass library object
 * \param buf pointer to subtitles text
 * \param bufsize size of buffer
 * \param codepage recode buffer contents from given codepage
 * \return newly allocated track
*/
ASS_Track *ass_read_memory(ASS_Library *library, char *buf,
                           size_t bufsize, char *codepage)
{
    ASS_Track *track;
    int copied = 0;

    if (!buf)
        return 0;

#ifdef CONFIG_ICONV
    if (codepage) {
        buf = sub_recode(library, buf, bufsize, codepage);
        if (!buf)
            return 0;
        else
            copied = 1;
    }
#endif
    if (!copied) {
        char *newbuf = malloc(bufsize + 1);
        if (!newbuf)
            return 0;
        memcpy(newbuf, buf, bufsize);
        newbuf[bufsize] = '\0';
        buf = newbuf;
    }
    track = parse_memory(library, buf);
    free(buf);
    if (!track)
        return 0;

    ass_msg(library, MSGL_INFO, "Added subtitle file: "
            "<memory> (%d styles, %d events)",
            track->n_styles, track->n_events);
    return track;
}

static char *read_file_recode(ASS_Library *library, char *fname,
                              char *codepage, size_t *size)
{
    char *buf;
    size_t bufsize;

    buf = read_file(library, fname, &bufsize);
    if (!buf)
        return 0;
#ifdef CONFIG_ICONV
    if (codepage) {
        char *tmpbuf = sub_recode(library, buf, bufsize, codepage);
        free(buf);
        buf = tmpbuf;
    }
    if (!buf)
        return 0;
#endif
    *size = bufsize;
    return buf;
}

/**
 * \brief Read subtitles from file.
 * \param library libass library object
 * \param fname file name
 * \param codepage recode buffer contents from given codepage
 * \return newly allocated track
*/
ASS_Track *ass_read_file(ASS_Library *library, char *fname,
                         char *codepage)
{
    char *buf;
    ASS_Track *track;
    size_t bufsize;

    buf = read_file_recode(library, fname, codepage, &bufsize);
    if (!buf)
        return 0;
    track = parse_memory(library, buf);
    free(buf);
    if (!track)
        return 0;

    track->name = strdup(fname);

    ass_msg(library, MSGL_INFO,
            "Added subtitle file: '%s' (%d styles, %d events)",
            fname, track->n_styles, track->n_events);

    return track;
}

/**
 * \brief read styles from file into already initialized track
 */
int ass_read_styles(ASS_Track *track, char *fname, char *codepage)
{
    char *buf;
    ParserState old_state;
    size_t sz;

    buf = read_file(track->library, fname, &sz);
    if (!buf)
        return 1;
#ifdef CONFIG_ICONV
    if (codepage) {
        char *tmpbuf;
        tmpbuf = sub_recode(track->library, buf, sz, codepage);
        free(buf);
        buf = tmpbuf;
    }
    if (!buf)
        return 1;
#endif

    old_state = track->parser_priv->state;
    track->parser_priv->state = PST_STYLES;
    process_text(track, buf);
    free(buf);
    track->parser_priv->state = old_state;

    return 0;
}

long long ass_step_sub(ASS_Track *track, long long now, int movement)
{
    int i;
    ASS_Event *best = NULL;
    long long target = now;
    int direction = (movement > 0 ? 1 : -1) * !!movement;

    if (track->n_events == 0)
        return 0;

    do {
        ASS_Event *closest = NULL;
        long long closest_time = now;
        for (i = 0; i < track->n_events; i++) {
            if (direction < 0) {
                long long end =
                    track->events[i].Start + track->events[i].Duration;
                if (end < target) {
                    if (!closest || end > closest_time) {
                        closest = &track->events[i];
                        closest_time = end;
                    }
                }
            } else if (direction > 0) {
                long long start = track->events[i].Start;
                if (start > target) {
                    if (!closest || start < closest_time) {
                        closest = &track->events[i];
                        closest_time = start;
                    }
                }
            } else {
                long long start = track->events[i].Start;
                if (start < target) {
                    if (!closest || start >= closest_time) {
                        closest = &track->events[i];
                        closest_time = start;
                    }
                }
            }
        }
        target = closest_time + direction;
        movement -= direction;
        if (closest)
            best = closest;
    } while (movement);

    return best ? best->Start - now : 0;
}

ASS_Track *ass_new_track(ASS_Library *library)
{
    int def_sid = -1;
    ASS_Track *track = calloc(1, sizeof(ASS_Track));
    if (!track)
        goto fail;
    track->library = library;
    track->ScaledBorderAndShadow = 0;
    track->parser_priv = calloc(1, sizeof(ASS_ParserPriv));
    if (!track->parser_priv)
        goto fail;
    def_sid = ass_alloc_style(track);
    if (def_sid < 0)
        goto fail;
    set_default_style(track->styles + def_sid);
    track->default_style = def_sid;
    if (!track->styles[def_sid].Name || !track->styles[def_sid].FontName)
        goto fail;
    track->parser_priv->event_format.max_tokens = ASS_FORMAT_ALLOC;
    track->parser_priv->style_format.max_tokens = ASS_FORMAT_ALLOC;
    if (!ASS_REALLOC_ARRAY(track->parser_priv->event_format.tokens, ASS_FORMAT_ALLOC) ||
            !ASS_REALLOC_ARRAY(track->parser_priv->style_format.tokens, ASS_FORMAT_ALLOC))
        goto fail;
    track->parser_priv->check_readorder = 1;
    return track;

fail:
    if (track) {
        if (def_sid >= 0)
            ass_free_style(track, def_sid);
        if (track->parser_priv) {
            free(track->parser_priv->style_format.tokens);
            free(track->parser_priv->event_format.tokens);
            free(track->parser_priv);
        }
        free(track);
    }
    return NULL;
}

int ass_track_set_feature(ASS_Track *track, ASS_Feature feature, int enable)
{
    switch (feature) {
    case ASS_FEATURE_INCOMPATIBLE_EXTENSIONS:
        //-fallthrough
#ifdef USE_FRIBIDI_EX_API
    case ASS_FEATURE_BIDI_BRACKETS:
        track->parser_priv->bidi_brackets = !!enable;
#endif
        return 0;
    default:
        return -1;
    }
}

/**
 * \brief Prepare track for rendering
 */
void ass_lazy_track_init(ASS_Library *lib, ASS_Track *track)
{
    if (track->PlayResX > 0 && track->PlayResY > 0)
        return;
    if (track->PlayResX <= 0 && track->PlayResY <= 0) {
        ass_msg(lib, MSGL_WARN,
               "Neither PlayResX nor PlayResY defined. Assuming 384x288");
        track->PlayResX = 384;
        track->PlayResY = 288;
    } else {
        if (track->PlayResY <= 0 && track->PlayResX == 1280) {
            track->PlayResY = 1024;
            ass_msg(lib, MSGL_WARN,
                   "PlayResY undefined, setting to %d", track->PlayResY);
        } else if (track->PlayResY <= 0) {
            track->PlayResY = FFMAX(1, track->PlayResX * 3LL / 4);
            ass_msg(lib, MSGL_WARN,
                   "PlayResY undefined, setting to %d", track->PlayResY);
        } else if (track->PlayResX <= 0 && track->PlayResY == 1024) {
            track->PlayResX = 1280;
            ass_msg(lib, MSGL_WARN,
                   "PlayResX undefined, setting to %d", track->PlayResX);
        } else if (track->PlayResX <= 0) {
            track->PlayResX = FFMAX(1, track->PlayResY * 4LL / 3);
            ass_msg(lib, MSGL_WARN,
                   "PlayResX undefined, setting to %d", track->PlayResX);
        }
    }
}
