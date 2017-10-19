/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2017 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* $Id: music_mod.c 4211 2008-12-08 00:27:32Z slouken $ */

#ifdef MOD_MUSIC

/* This file supports MOD tracker music streams */

#include <SDL_mixer_ext/SDL_mixer_ext.h>
#include "dynamic_mod.h"
#include "music_mod.h"

#include "mikmod.h"

#define SDL_SURROUND
#ifdef SDL_SURROUND
#define MAX_OUTPUT_CHANNELS 6
#else
#define MAX_OUTPUT_CHANNELS 2
#endif

/* Reference for converting mikmod output to 4/6 channels */
static int      current_output_channels;
static Uint16   current_output_format;

static int music_swap8;
static int music_swap16;

/* Load a MOD stream from an SDL_RWops object */
static void *MOD_new_RW(SDL_RWops *rw, int freerw);
/* Close the given MOD stream */
static void MOD_delete(void *music_p);

/* Set the volume for a MOD stream */
static void MOD_setvolume(void *music_p, int volume);

/* Start playback of a given MOD stream */
static void MOD_play(void *music_p);
/* Stop playback of a stream previously started with MOD_play() */
static void MOD_stop(void *music_p);

/* Return non-zero if a stream is currently playing */
static int MOD_playing(void *music_p);

/* Jump (seek) to a given position (time is in seconds) */
static void MOD_jump_to_time(void *music_p, double time);

/* Play some of a stream previously started with MOD_play() */
static int MOD_playAudio(void *music_p, Uint8 *stream, int len);

static const char *MOD_metaTitle(void *music_p);

static Uint32      MOD_Codec_capabilities()
{
    return ACODEC_NEED_VOLUME_INIT_POST|ACODEC_SINGLETON;
}

/* Initialize the MOD player, with the given mixer settings
   This function returns 0, or -1 if there was an error.
 */
int MOD_init2(AudioCodec *codec, SDL_AudioSpec *mixerfmt)
{
    CHAR *list;

    if(!Mix_Init(MIX_INIT_MOD))
    {
        return -1;
    }

    /* Set the MikMod music format */
    music_swap8 = 0;
    music_swap16 = 0;
    switch(mixerfmt->format)
    {

    case AUDIO_U8:
    case AUDIO_S8:
        {
            if(mixerfmt->format == AUDIO_S8)
            {
                music_swap8 = 1;
            }
            *mikmod.md_mode = 0;
        }
    break;

    case AUDIO_S16LSB:
    case AUDIO_S16MSB:
        {
            /* See if we need to correct MikMod mixing */
            #if SDL_BYTEORDER == SDL_LIL_ENDIAN
            #define MIKMOD_AUDIO AUDIO_S16MSB
            #else
            #define MIKMOD_AUDIO AUDIO_S16LSB
            #endif
            if(mixerfmt->format == MIKMOD_AUDIO)
            {
                music_swap16 = 1;
            }
            *mikmod.md_mode = DMODE_16BITS;
            #undef MIKMOD_AUDIO
        }
    break;

    default:
        {
            Mix_SetError("Unknown hardware audio format");
            return -1;
        }
    }
    current_output_channels = mixerfmt->channels;
    current_output_format = mixerfmt->format;
    if(mixerfmt->channels > 1)
    {
        if(mixerfmt->channels > MAX_OUTPUT_CHANNELS)
        {
            Mix_SetError("Hardware uses more channels than mixerfmt");
            return -1;
        }
        *mikmod.md_mode |= DMODE_STEREO;
    }
    *mikmod.md_mixfreq = (UWORD)mixerfmt->freq;
    *mikmod.md_device  = 0;
    *mikmod.md_volume  = 96;
    *mikmod.md_musicvolume = 128;
    *mikmod.md_sndfxvolume = 128;
    *mikmod.md_pansep  = 128;
    *mikmod.md_reverb  = 0;
    *mikmod.md_mode    |= DMODE_HQMIXER | DMODE_SOFT_MUSIC | DMODE_SURROUND;

    list = mikmod.MikMod_InfoDriver();
    if(list)
        mikmod.MikMod_free(list);
    else
        mikmod.MikMod_RegisterDriver(mikmod.drv_nos);

    list = mikmod.MikMod_InfoLoader();
    if(list)
        mikmod.MikMod_free(list);
    else
        mikmod.MikMod_RegisterAllLoaders();

    if(mikmod.MikMod_Init(NULL))
    {
        Mix_SetError("%s", mikmod.MikMod_strerror(*mikmod.MikMod_errno));
        return -1;
    }

    codec->isValid = 1;

    codec->capabilities     = MOD_Codec_capabilities;

    codec->open             = MOD_new_RW;
    codec->openEx           = audioCodec_dummy_cb_openEx;
    codec->close            = MOD_delete;

    codec->play             = MOD_play;
    codec->pause            = audioCodec_dummy_cb_void_1arg;
    codec->resume           = audioCodec_dummy_cb_void_1arg;
    codec->stop             = MOD_stop;

    codec->isPlaying        = MOD_playing;
    codec->isPaused         = audioCodec_dummy_cb_int_1arg;

    codec->setLoops         = audioCodec_dummy_cb_regulator;
    codec->setVolume        = MOD_setvolume;

    codec->jumpToTime       = MOD_jump_to_time;
    codec->getCurrentTime   = audioCodec_dummy_cb_tell;

    codec->metaTitle        = MOD_metaTitle;
    codec->metaArtist       = audioCodec_dummy_meta_tag;
    codec->metaAlbum        = audioCodec_dummy_meta_tag;
    codec->metaCopyright    = audioCodec_dummy_meta_tag;

    codec->playAudio        = MOD_playAudio;

    return 0;
}

/* Uninitialize the music players */
void MOD_exit(void)
{
    if(mikmod.MikMod_Exit)
    {
        mikmod.MikMod_Exit();
    }
}

/* Set the volume for a MOD stream */
static void MOD_setvolume(void *music_p, int volume)
{
    MODULE *music = (MODULE *)music_p;
    mikmod.Player_SetVolume((SWORD)volume);
}

typedef struct
{
    MREADER mr;
    /* struct MREADER in libmikmod <= 3.2.0-beta2
     * doesn't have iobase members. adding them here
     * so that if we compile against 3.2.0-beta2, we
     * can still run OK against 3.2.0b3 and newer. */
    long iobase, prev_iobase;
    Sint64 offset;
    Sint64 eof;
    SDL_RWops *src;
} LMM_MREADER;

int LMM_Seek(struct MREADER *mr, long to, int dir)
{
    Sint64 offset = to;
    LMM_MREADER* lmmmr = (LMM_MREADER*)mr;
    if ( dir == SEEK_SET ) {
        offset += lmmmr->offset;
        if (offset < lmmmr->offset)
            return -1;
    }
    return (int)(SDL_RWseek(lmmmr->src, offset, dir));
}
long LMM_Tell(struct MREADER *mr)
{
    LMM_MREADER *lmmmr = (LMM_MREADER *)mr;
    return (long)(SDL_RWtell(lmmmr->src) - lmmmr->offset);
}

BOOL LMM_Read(struct MREADER *mr, void *buf, size_t sz)
{
    LMM_MREADER *lmmmr = (LMM_MREADER *)mr;
    return SDL_RWread(lmmmr->src, buf, sz, 1);
}

int LMM_Get(struct MREADER *mr)
{
    unsigned char c;
    LMM_MREADER *lmmmr = (LMM_MREADER *)mr;
    if(SDL_RWread(lmmmr->src, &c, 1, 1))
    {
        return c;
    }
    return EOF;
}

BOOL LMM_Eof(struct MREADER *mr)
{
    Sint64 offset;
    LMM_MREADER *lmmmr = (LMM_MREADER *)mr;
    offset = LMM_Tell(mr);
    return offset >= lmmmr->eof;
}

MODULE *MikMod_LoadSongRW(SDL_RWops *src, int maxchan)
{
    LMM_MREADER lmmmr =
    {
        { LMM_Seek, LMM_Tell, LMM_Read, LMM_Get, LMM_Eof },
        0,
        0,
        0
    };
    lmmmr.offset = SDL_RWtell(src);
    SDL_RWseek(src, 0, RW_SEEK_END);
    lmmmr.eof = SDL_RWtell(src);
    SDL_RWseek(src, lmmmr.offset, RW_SEEK_SET);
    lmmmr.src = src;
    return mikmod.Player_LoadGeneric((MREADER *)&lmmmr, maxchan, 0);
}

/* Load a MOD stream from an SDL_RWops object */
static void *MOD_new_RW(SDL_RWops *src, int freesrc)
{
    MODULE *module;

    /* Make sure the mikmod library is loaded */
    if(!Mix_Init(MIX_INIT_MOD))
    {
        return NULL;
    }

    module = MikMod_LoadSongRW(src, 64);
    if(!module)
    {
        Mix_SetError("%s", mikmod.MikMod_strerror(*mikmod.MikMod_errno));
        return NULL;
    }

    /* Stop implicit looping, fade out and other flags. */
    module->extspd  = 1;
    module->panflag = 1;
    module->wrap    = 0;
    module->loop    = 1;
    #if 0 /* Don't set fade out by default - unfortunately there's no real way
    to query the status of the song or set trigger actions.  Hum. */
    module->fadeout = 1;
    #endif

    if(freesrc)
    {
        SDL_RWclose(src);
    }
    return module;
}

/* Start playback of a given MOD stream */
static void MOD_play(void *music_p)
{
    MODULE *music = (MODULE *)music_p;
    mikmod.Player_Start(music);
}

/* Return non-zero if a stream is currently playing */
static int MOD_playing(void *music_p)
{
    MODULE *music = (MODULE *)music_p;
    return mikmod.Player_Active();
}

/* Play some of a stream previously started with MOD_play() */
static int MOD_playAudio(void *music_p, Uint8 *stream, int len)
{
    MODULE *music = (MODULE *)music_p;
    if(current_output_channels > 2)
    {
        int small_len = 2 * len / current_output_channels;
        int i;
        Uint8 *src, *dst;

        mikmod.VC_WriteBytes((SBYTE *)stream, small_len);
        /* and extend to len by copying channels */
        src = stream + small_len;
        dst = stream + len;

        switch(current_output_format & 0xFF)
        {
        case 8:
            for(i = small_len / 2; i; --i)
            {
                src -= 2;
                dst -= current_output_channels;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[1];
                if(current_output_channels == 6)
                {
                    dst[4] = src[0];
                    dst[5] = src[1];
                }
            }
            break;
        case 16:
            for(i = small_len / 4; i; --i)
            {
                src -= 4;
                dst -= 2 * current_output_channels;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
                dst[4] = src[0];
                dst[5] = src[1];
                dst[6] = src[2];
                dst[7] = src[3];
                if(current_output_channels == 6)
                {
                    dst[8] = src[0];
                    dst[9] = src[1];
                    dst[10] = src[2];
                    dst[11] = src[3];
                }
            }
            break;
        }
    }
    else
    {
        mikmod.VC_WriteBytes((SBYTE *)stream, len);
    }
    if(music_swap8)
    {
        Uint8 *dst;
        int i;

        dst = stream;
        for(i = len; i; --i)
        {
            *dst++ ^= 0x80;
        }
    }
    else if(music_swap16)
    {
        Uint8 *dst, tmp;
        int i;

        dst = stream;
        for(i = (len / 2); i; --i)
        {
            tmp = dst[0];
            dst[0] = dst[1];
            dst[1] = tmp;
            dst += 2;
        }
    }
    return 0;
}

/* Stop playback of a stream previously started with MOD_play() */
static void MOD_stop(void *music_p)
{
    MODULE *music = (MODULE *)music_p;
    mikmod.Player_Stop();
}

/* Close the given MOD stream */
static void MOD_delete(void *music_p)
{
    MODULE *music = (MODULE *)music_p;
    mikmod.Player_Free(music);
}

static const char *MOD_metaTitle(void *music_p)
{
    MODULE *music = (MODULE *)music_p;
    return music->comment;
}

/* Jump (seek) to a given position (time is in seconds) */
static void MOD_jump_to_time(void *music_p, double time)
{
    MODULE *music = (MODULE *)music_p;
    mikmod.Player_SetPosition((UWORD)time);
}

#endif /* MOD_MUSIC */
