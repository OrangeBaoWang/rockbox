/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 Robert E. Hak
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <string.h>
#include "lcd.h"
#include "font.h"
#include "kernel.h"
#include "rtc.h"
#include "powermgmt.h"

#include "settings.h"

#include "metadata.h"
#include "icons.h"

const unsigned char bitmap_icons_5x8[][5] =
{
    [Icon_Lock_Main] =
        {0x78, 0x7f, 0x49, 0x7f, 0x78}, /* Lock Main */
    [Icon_Lock_Remote] =
        {0x78, 0x7f, 0x49, 0x7f, 0x78}, /* Lock Remote */
    [Icon_Stereo] =
        {0x7f, 0x22, 0x1c, 0x22, 0x7f}, /* Stereo recording */
    [Icon_Mono] =
        {0x00, 0x1c, 0x22, 0x7f, 0x00}, /* Mono recording */
#if CONFIG_CODEC != SWCODEC
    [Icon_q] =
        {0x1e, 0x21, 0x31, 0x21, 0x5e}  /* Q icon */
#endif
};

const unsigned char bitmap_icons_7x8[][7] =
{
    {0x08,0x1c,0x3e,0x3e,0x3e,0x14,0x14}, /* Power plug */
    {0x1c,0x14,0x3e,0x2a,0x22,0x1c,0x08}, /* USB plug */
    {0x01,0x1e,0x1c,0x3e,0x7f,0x20,0x40}, /* Speaker mute */
    {0x00,0x7f,0x7f,0x3e,0x1c,0x08,0x00}, /* Play */
    {0x7f,0x7f,0x7f,0x7f,0x7f,0x7f,0x7f}, /* Stop */
    {0x00,0x7f,0x7f,0x00,0x7f,0x7f,0x00}, /* Pause */
    {0x7f,0x3e,0x1c,0x7f,0x3e,0x1c,0x08}, /* Fast forward */
    {0x08,0x1c,0x3e,0x7f,0x1c,0x3e,0x7f}, /* Fast backward */
    {0x1c,0x3e,0x7f,0x7f,0x7f,0x3e,0x1c}, /* Record */
    {0x1c,0x3e,0x7f,0x00,0x7f,0x3e,0x1c}, /* Record pause */
    {0x40,0xa0,0xa0,0xa0,0x7f,0x02,0x02}, /* Radio on */
    {0x42,0xa4,0xa8,0xb0,0x7f,0x22,0x42}, /* Radio mute */
    {0x44,0x4e,0x5f,0x44,0x44,0x44,0x38}, /* Repeat playmode */
    {0x44,0x4e,0x5f,0x44,0x38,0x02,0x7f}, /* Repeat-one playmode */
    {0x3e,0x41,0x51,0x41,0x45,0x41,0x3e}, /* Shuffle playmode (dice) */
    {0x04,0x0c,0x1c,0x3c,0x1c,0x0c,0x04}, /* Down-arrow */
    {0x20,0x30,0x38,0x3c,0x38,0x30,0x20}, /* Up-arrow */
    {0x7f,0x04,0x4e,0x5f,0x44,0x38,0x7f}  /* Repeat-AB playmode */
};

#if CONFIG_CODEC == SWCODEC && defined(HAVE_RECORDING)
const unsigned char bitmap_glyphs_4x8[][4] =
{
    /* Keep digits together and first! */
    [0] =
        {0x00, 0x3e, 0x22, 0x3e}, /* 0 */
    [1] =
        {0x00, 0x24, 0x3e, 0x20}, /* 1 */
    [2] =
        {0x00, 0x3a, 0x2a, 0x2e}, /* 2 */
    [3] =
        {0x00, 0x22, 0x2a, 0x36}, /* 3 */
    [4] =
        {0x00, 0x0e, 0x08, 0x3e}, /* 4 */
    [5] =
        {0x00, 0x2e, 0x2a, 0x3a}, /* 5 */
    [6] =
        {0x00, 0x3e, 0x2a, 0x3a}, /* 6 */
    [7] =
        {0x00, 0x02, 0x02, 0x3e}, /* 7 */
    [8] =
        {0x00, 0x3e, 0x2a, 0x3e}, /* 8 */
    [9] =
        {0x00, 0x0e, 0x0a, 0x3e}, /* 9 */
    [10 ... Glyph_4x8Last-1] =
        {0x00, 0x00, 0x00, 0x00}, /* auto-blank  */
    [Glyph_4x8_k] =
        {0x00, 0x3e, 0x10, 0x28}, /* k */
};

const unsigned char bitmap_formats_18x8[Format_18x8Last][18]=
{
    [0 ... Format_18x8Last-1] =   /* auto-blank */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ___ */
    [Format_18x8_AIFF] =
        {0x00, 0x3c, 0x0a, 0x0a, 0x0a, 0x3c, 0x00, 0x3e, 0x00,
         0x3e, 0x0a, 0x02, 0x02, 0x00, 0x3e, 0x0a, 0x02, 0x02}, /* AIFF */
    [Format_18x8_MPA_L3] =
        {0x00, 0x3e, 0x04, 0x08, 0x04, 0x3e, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* M__ */
    [Format_18x8_WAVPACK] =
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x20,
         0x18, 0x20, 0x1e, 0x00, 0x0e, 0x10, 0x20, 0x10, 0x0e}, /* _WV */
    [Format_18x8_PCM_WAV] =
        {0x00, 0x1e, 0x20, 0x18, 0x20, 0x1e, 0x00, 0x3c, 0x0a,
         0x0a, 0x0a, 0x3c, 0x00, 0x0e, 0x10, 0x20, 0x10, 0x0e}, /* WAV */
};
#endif /* CONFIG_CODEC == SWCODEC && defined(HAVE_RECORDING) */

/* Disk/MMC activity */
const unsigned char bitmap_icon_disk[12] =
#ifdef HAVE_FLASH_STORAGE
    {0x15,0x3f,0x7d,0x7B,0x77,0x67,0x79,0x7b,0x57,0x4f,0x47,0x7f};
#else
    {0x00,0x00,0x00,0x1c,0x2e,0x4f,0x77,0x79,0x3a,0x1c,0x00,0x00};
#endif

/*
 * Print play mode to status bar
 */
void statusbar_icon_play_mode(int mode)
{
    lcd_mono_bitmap(bitmap_icons_7x8[mode], ICON_PLAY_MODE_X_POS,
                    STATUSBAR_Y_POS, ICON_PLAY_MODE_WIDTH, STATUSBAR_HEIGHT);
}

/*
 * Print shuffle mode to status bar
 */
void statusbar_icon_shuffle(void)
{
    lcd_mono_bitmap(bitmap_icons_7x8[Icon_Shuffle], ICON_SHUFFLE_X_POS,
                    STATUSBAR_Y_POS, ICON_SHUFFLE_WIDTH, STATUSBAR_HEIGHT);
}
