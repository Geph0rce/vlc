/**
 * @file rtp.c
 * @brief Real-Time Protocol (RTP) demux module for VLC media player
 */
/*****************************************************************************
 * Copyright (C) 2001-2005 VLC authors and VideoLAN
 * Copyright © 2007-2009 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <stdarg.h>
#include <assert.h>

#include <vlc_common.h>
#include <vlc_demux.h>
#include <vlc_network.h>
#include <vlc_plugin.h>

#include "rtp.h"
#ifdef HAVE_SRTP
# include "srtp.h"
# include <gcrypt.h>
# include <vlc_gcrypt.h>
#endif

/*
 * TODO: so much stuff
 * - send RTCP-RR and RTCP-BYE
 * - dynamic payload types (need SDP parser)
 * - multiple medias (need SDP parser, and RTCP-SR parser for lip-sync)
 * - support for stream_filter in case of chained demux (MPEG-TS)
 */

#ifndef IPPROTO_DCCP
# define IPPROTO_DCCP 33 /* IANA */
#endif

#ifndef IPPROTO_UDPLITE
# define IPPROTO_UDPLITE 136 /* from IANA */
#endif

/**
 * Extracts port number from "[host]:port" or "host:port" strings,
 * and remove brackets from the host name.
 * @param phost pointer to the string upon entry,
 * pointer to the hostname upon return.
 * @return port number, 0 if missing.
 */
static int extract_port (char **phost)
{
    char *host = *phost, *port;

    if (host[0] == '[')
    {
        host = ++*phost; /* skip '[' */
        port = strchr (host, ']');
        if (port)
            *port++ = '\0'; /* skip ']' */
    }
    else
        port = strchr (host, ':');

    if (port == NULL)
        return 0;
    *port++ = '\0'; /* skip ':' */
    return atoi (port);
}

/**
 * Control callback
 */
static int Control (demux_t *demux, int query, va_list args)
{
    demux_sys_t *sys = demux->p_sys;

    switch (query)
    {
        case DEMUX_GET_PTS_DELAY:
        {
            *va_arg (args, vlc_tick_t *) =
                VLC_TICK_FROM_MS( var_InheritInteger (demux, "network-caching") );
            return VLC_SUCCESS;
        }

        case DEMUX_CAN_PAUSE:
        case DEMUX_CAN_SEEK:
        case DEMUX_CAN_CONTROL_PACE:
        {
            bool *v = va_arg( args, bool * );
            *v = false;
            return VLC_SUCCESS;
        }
    }

    if (sys->chained_demux != NULL)
        return vlc_demux_chained_ControlVa (sys->chained_demux, query, args);

    switch (query)
    {
        case DEMUX_GET_POSITION:
        {
            float *v = va_arg (args, float *);
            *v = 0.;
            return VLC_SUCCESS;
        }

        case DEMUX_GET_LENGTH:
        case DEMUX_GET_TIME:
        {
            *va_arg (args, vlc_tick_t *) = 0;
            return VLC_SUCCESS;
        }
    }

    return VLC_EGENERIC;
}

/**
 * Releases resources
 */
static void Close (vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    demux_sys_t *p_sys = demux->p_sys;

    if (p_sys->thread_ready)
    {
        vlc_cancel (p_sys->thread);
        vlc_join (p_sys->thread, NULL);
    }

#ifdef HAVE_SRTP
    if (p_sys->srtp)
        srtp_destroy (p_sys->srtp);
#endif
    if (p_sys->session)
        rtp_session_destroy (demux, p_sys->session);
    if (p_sys->rtcp_fd != -1)
        net_Close (p_sys->rtcp_fd);
    net_Close (p_sys->fd);
    free (p_sys);
}

/**
 * Probes and initializes.
 */
static int Open (vlc_object_t *obj)
{
    demux_t *demux = (demux_t *)obj;
    int tp; /* transport protocol */

    if (demux->out == NULL)
        return VLC_EGENERIC;

    if (!strcasecmp(demux->psz_name, "dccp"))
        tp = IPPROTO_DCCP;
    else
    if (!strcasecmp(demux->psz_name, "rtptcp"))
        tp = IPPROTO_TCP;
    else
    if (!strcasecmp(demux->psz_name, "rtp"))
        tp = IPPROTO_UDP;
    else
    if (!strcasecmp(demux->psz_name, "udplite"))
        tp = IPPROTO_UDPLITE;
    else
        return VLC_EGENERIC;

    char *tmp = strdup (demux->psz_location);
    if (tmp == NULL)
        return VLC_ENOMEM;

    char *shost;
    char *dhost = strchr (tmp, '@');
    if (dhost != NULL)
    {
        *(dhost++) = '\0';
        shost = tmp;
    }
    else
    {
        dhost = tmp;
        shost = NULL;
    }

    /* Parses the port numbers */
    int sport = 0, dport = 0;
    if (shost != NULL)
        sport = extract_port (&shost);
    if (dhost != NULL)
        dport = extract_port (&dhost);
    if (dport == 0)
        dport = 5004; /* avt-profile-1 port */

    int rtcp_dport = var_CreateGetInteger (obj, "rtcp-port");

    /* Try to connect */
    int fd = -1, rtcp_fd = -1;

    switch (tp)
    {
        case IPPROTO_UDP:
        case IPPROTO_UDPLITE:
            fd = net_OpenDgram (obj, dhost, dport, shost, sport, tp);
            if (fd == -1)
                break;
            if (rtcp_dport > 0) /* XXX: source port is unknown */
                rtcp_fd = net_OpenDgram (obj, dhost, rtcp_dport, shost, 0, tp);
            break;

         case IPPROTO_DCCP:
#ifndef SOCK_DCCP /* provisional API (FIXME) */
# ifdef __linux__
#  define SOCK_DCCP 6
# endif
#endif
#ifdef SOCK_DCCP
            var_Create (obj, "dccp-service", VLC_VAR_STRING);
            var_SetString (obj, "dccp-service", "RTPV"); /* FIXME: RTPA? */
            fd = net_Connect (obj, dhost, dport, SOCK_DCCP, tp);
#else
            msg_Err (obj, "DCCP support not included");
#endif
            break;

        case IPPROTO_TCP:
            fd = net_Connect (obj, dhost, dport, SOCK_STREAM, tp);
            break;
    }

    free (tmp);
    if (fd == -1)
        return VLC_EGENERIC;
    net_SetCSCov (fd, -1, 12);

    /* Initializes demux */
    demux_sys_t *p_sys = malloc (sizeof (*p_sys));
    if (p_sys == NULL)
    {
        net_Close (fd);
        if (rtcp_fd != -1)
            net_Close (rtcp_fd);
        return VLC_EGENERIC;
    }

    p_sys->chained_demux = NULL;
#ifdef HAVE_SRTP
    p_sys->srtp         = NULL;
#endif
    p_sys->fd           = fd;
    p_sys->rtcp_fd      = rtcp_fd;
    p_sys->max_src      = var_CreateGetInteger (obj, "rtp-max-src");
    p_sys->timeout      = vlc_tick_from_sec( var_CreateGetInteger (obj, "rtp-timeout") );
    p_sys->max_dropout  = var_CreateGetInteger (obj, "rtp-max-dropout");
    p_sys->max_misorder = var_CreateGetInteger (obj, "rtp-max-misorder");
    p_sys->thread_ready = false;
    p_sys->autodetect   = true;

    demux->pf_demux   = NULL;
    demux->pf_control = Control;
    demux->p_sys      = p_sys;

    p_sys->session = rtp_session_create (demux);
    if (p_sys->session == NULL)
        goto error;

#ifdef HAVE_SRTP
    char *key = var_CreateGetNonEmptyString (demux, "srtp-key");
    if (key)
    {
        vlc_gcrypt_init ();
        p_sys->srtp = srtp_create (SRTP_ENCR_AES_CM, SRTP_AUTH_HMAC_SHA1, 10,
                                   SRTP_PRF_AES_CM, SRTP_RCC_MODE1);
        if (p_sys->srtp == NULL)
        {
            free (key);
            goto error;
        }

        char *salt = var_CreateGetNonEmptyString (demux, "srtp-salt");
        int val = srtp_setkeystring (p_sys->srtp, key, salt ? salt : "");
        free (salt);
        free (key);
        if (val)
        {
            msg_Err (obj, "bad SRTP key/salt combination (%s)",
                     vlc_strerror_c(val));
            goto error;
        }
    }
#endif

    if (vlc_clone (&p_sys->thread,
                   (tp != IPPROTO_TCP) ? rtp_dgram_thread : rtp_stream_thread,
                   demux, VLC_THREAD_PRIORITY_INPUT))
        goto error;
    p_sys->thread_ready = true;
    return VLC_SUCCESS;

error:
    Close (obj);
    return VLC_EGENERIC;
}

#define RTCP_PORT_TEXT N_("RTCP (local) port")
#define RTCP_PORT_LONGTEXT N_( \
    "RTCP packets will be received on this transport protocol port. " \
    "If zero, multiplexed RTP/RTCP is used.")

#define SRTP_KEY_TEXT N_("SRTP key (hexadecimal)")
#define SRTP_KEY_LONGTEXT N_( \
    "RTP packets will be authenticated and deciphered "\
    "with this Secure RTP master shared secret key. "\
    "This must be a 32-character-long hexadecimal string.")

#define SRTP_SALT_TEXT N_("SRTP salt (hexadecimal)")
#define SRTP_SALT_LONGTEXT N_( \
    "Secure RTP requires a (non-secret) master salt value. " \
    "This must be a 28-character-long hexadecimal string.")

#define RTP_MAX_SRC_TEXT N_("Maximum RTP sources")
#define RTP_MAX_SRC_LONGTEXT N_( \
    "How many distinct active RTP sources are allowed at a time." )

#define RTP_TIMEOUT_TEXT N_("RTP source timeout (sec)")
#define RTP_TIMEOUT_LONGTEXT N_( \
    "How long to wait for any packet before a source is expired.")

#define RTP_MAX_DROPOUT_TEXT N_("Maximum RTP sequence number dropout")
#define RTP_MAX_DROPOUT_LONGTEXT N_( \
    "RTP packets will be discarded if they are too much ahead (i.e. in the " \
    "future) by this many packets from the last received packet." )

#define RTP_MAX_MISORDER_TEXT N_("Maximum RTP sequence number misordering")
#define RTP_MAX_MISORDER_LONGTEXT N_( \
    "RTP packets will be discarded if they are too far behind (i.e. in the " \
    "past) by this many packets from the last received packet." )

#define RTP_DYNAMIC_PT_TEXT N_("RTP payload format assumed for dynamic " \
                               "payloads")
#define RTP_DYNAMIC_PT_LONGTEXT N_( \
    "This payload format will be assumed for dynamic payload types " \
    "(between 96 and 127) if it can't be determined otherwise with " \
    "out-of-band mappings (SDP)" )

static const char *const dynamic_pt_list[] = { "theora" };
static const char *const dynamic_pt_list_text[] = { "Theora Encoded Video" };

/*
 * Module descriptor
 */
vlc_module_begin()
    set_shortname(N_("RTP"))
    set_description(N_("Real-Time Protocol (RTP) input"))
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_DEMUX)
    set_capability("access", 0)
    set_callbacks(Open, Close)

    add_integer("rtcp-port", 0, RTCP_PORT_TEXT,
                 RTCP_PORT_LONGTEXT, false)
        change_integer_range(0, 65535)
        change_safe()
#ifdef HAVE_SRTP
    add_string ("srtp-key", "",
                SRTP_KEY_TEXT, SRTP_KEY_LONGTEXT, false)
        change_safe ()
    add_string("srtp-salt", "",
               SRTP_SALT_TEXT, SRTP_SALT_LONGTEXT, false)
        change_safe()
#endif
    add_integer("rtp-max-src", 1, RTP_MAX_SRC_TEXT,
                RTP_MAX_SRC_LONGTEXT, true)
        change_integer_range (1, 255)
    add_integer("rtp-timeout", 5, RTP_TIMEOUT_TEXT,
                RTP_TIMEOUT_LONGTEXT, true)
    add_integer("rtp-max-dropout", 3000, RTP_MAX_DROPOUT_TEXT,
                RTP_MAX_DROPOUT_LONGTEXT, true)
        change_integer_range (0, 32767)
    add_integer("rtp-max-misorder", 100, RTP_MAX_MISORDER_TEXT,
                RTP_MAX_MISORDER_LONGTEXT, true)
        change_integer_range (0, 32767)
    add_string("rtp-dynamic-pt", NULL, RTP_DYNAMIC_PT_TEXT,
               RTP_DYNAMIC_PT_LONGTEXT, true)
        change_string_list(dynamic_pt_list, dynamic_pt_list_text)

    /*add_shortcut ("sctp")*/
    add_shortcut("dccp", "rtptcp", /* "tcp" is already taken :( */
                 "rtp", "udplite")
vlc_module_end()
