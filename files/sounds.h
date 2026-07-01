// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// DESCRIPTION:
//	Created by the sound utility written by Dave Taylor.
//	Kept as a sample, DOOM2  sounds. Frozen.
//
//-----------------------------------------------------------------------------

#ifndef __SOUNDS__
#define __SOUNDS__


//
// SoundFX struct.
//
typedef struct sfxinfo_struct	sfxinfo_t;

struct sfxinfo_struct
{
    // up to 6-character name
    char*	name;

    // Sfx singularity (only one at a time)
    int		singularity;

    // Sfx priority
    int		priority;

    // referenced sound if a link
    sfxinfo_t*	link;

    // pitch if a link
    int		pitch;

    // volume if a link
    int		volume;

    // sound data
    void*	data;

    // this is checked every second to see if sound
    // can be thrown out (if 0, then decrement, if -1,
    // then throw out, if > 0, then it is in use)
    int		usefulness;

    // lump number of sfx
    int		lumpnum;		
};




//
// MusicInfo struct.
//
typedef struct
{
    // up to 6-character name
    char*	name;

    // lump number of music
    int		lumpnum;
    
    // music data
    void*	data;

    // music handle once registered
    int handle;
    
} musicinfo_t;




// the complete set of sound effects
extern sfxinfo_t	*S_sfx;
extern sfxinfo_t	S_sfx_builtin[];
extern int	num_sfx;

// the complete set of music
extern musicinfo_t	S_music[];

//
// Identifiers for all music in game.
//

typedef enum
{
    mus_None,
    mus_e1m1,
    mus_e1m2,
    mus_e1m3,
    mus_e1m4,
    mus_e1m5,
    mus_e1m6,
    mus_e1m7,
    mus_e1m8,
    mus_e1m9,
    mus_e2m1,
    mus_e2m2,
    mus_e2m3,
    mus_e2m4,
    mus_e2m5,
    mus_e2m6,
    mus_e2m7,
    mus_e2m8,
    mus_e2m9,
    mus_e3m1,
    mus_e3m2,
    mus_e3m3,
    mus_e3m4,
    mus_e3m5,
    mus_e3m6,
    mus_e3m7,
    mus_e3m8,
    mus_e3m9,
    mus_inter,
    mus_intro,
    mus_bunny,
    mus_victor,
    mus_introa,
    mus_runnin,
    mus_stalks,
    mus_countd,
    mus_betwee,
    mus_doom,
    mus_the_da,
    mus_shawn,
    mus_ddtblu,
    mus_in_cit,
    mus_dead,
    mus_stlks2,
    mus_theda2,
    mus_doom2,
    mus_ddtbl2,
    mus_runni2,
    mus_dead2,
    mus_stlks3,
    mus_romero,
    mus_shawn2,
    mus_messag,
    mus_count2,
    mus_ddtbl3,
    mus_ampie,
    mus_theda3,
    mus_adrian,
    mus_messg2,
    mus_romer2,
    mus_tense,
    mus_shawn3,
    mus_openin,
    mus_evil,
    mus_ultima,
    mus_read_m,
    mus_dm2ttl,
    mus_dm2int,
    NUMMUSIC
} musicenum_t;


//
// Identifiers for all sfx in game.
//

typedef enum
{
    sfx_None,
    sfx_pistol,
    sfx_shotgn,
    sfx_sgcock,
    sfx_dshtgn,
    sfx_dbopn,
    sfx_dbcls,
    sfx_dbload,
    sfx_plasma,
    sfx_bfg,
    sfx_sawup,
    sfx_sawidl,
    sfx_sawful,
    sfx_sawhit,
    sfx_rlaunc,
    sfx_rxplod,
    sfx_firsht,
    sfx_firxpl,
    sfx_pstart,
    sfx_pstop,
    sfx_doropn,
    sfx_dorcls,
    sfx_stnmov,
    sfx_swtchn,
    sfx_swtchx,
    sfx_plpain,
    sfx_dmpain,
    sfx_popain,
    sfx_vipain,
    sfx_mnpain,
    sfx_pepain,
    sfx_slop,
    sfx_itemup,
    sfx_wpnup,
    sfx_oof,
    sfx_telept,
    sfx_posit1,
    sfx_posit2,
    sfx_posit3,
    sfx_bgsit1,
    sfx_bgsit2,
    sfx_sgtsit,
    sfx_cacsit,
    sfx_brssit,
    sfx_cybsit,
    sfx_spisit,
    sfx_bspsit,
    sfx_kntsit,
    sfx_vilsit,
    sfx_mansit,
    sfx_pesit,
    sfx_sklatk,
    sfx_sgtatk,
    sfx_skepch,
    sfx_vilatk,
    sfx_claw,
    sfx_skeswg,
    sfx_pldeth,
    sfx_pdiehi,
    sfx_podth1,
    sfx_podth2,
    sfx_podth3,
    sfx_bgdth1,
    sfx_bgdth2,
    sfx_sgtdth,
    sfx_cacdth,
    sfx_skldth,
    sfx_brsdth,
    sfx_cybdth,
    sfx_spidth,
    sfx_bspdth,
    sfx_vildth,
    sfx_kntdth,
    sfx_pedth,
    sfx_skedth,
    sfx_posact,
    sfx_bgact,
    sfx_dmact,
    sfx_bspact,
    sfx_bspwlk,
    sfx_vilact,
    sfx_noway,
    sfx_barexp,
    sfx_punch,
    sfx_hoof,
    sfx_metal,
    sfx_chgun,
    sfx_tink,
    sfx_bdopn,
    sfx_bdcls,
    sfx_itmbk,
    sfx_flame,
    sfx_flamst,
    sfx_getpow,
    sfx_bospit,
    sfx_boscub,
    sfx_bossit,
    sfx_bospn,
    sfx_bosdth,
    sfx_manatk,
    sfx_mandth,
    sfx_sssit,
    sfx_ssdth,
    sfx_keenpn,
    sfx_keendt,
    sfx_skeact,
    sfx_skesit,
    sfx_skeatk,
    sfx_radio,
    // ---- Heretic monster sounds (DS* lumps from hereticstuff.wad) ----
    sfx_h_bstact,
    sfx_h_bstatk,
    sfx_h_bstdth,
    sfx_h_bstpai,
    sfx_h_bstsit,
    sfx_h_clkact,
    sfx_h_clkatk,
    sfx_h_clkdth,
    sfx_h_clkpai,
    sfx_h_clksit,
    sfx_h_hedact,
    sfx_h_hedat1,
    sfx_h_hedat2,
    sfx_h_hedat3,
    sfx_h_heddth,
    sfx_h_hedpai,
    sfx_h_hedsit,
    sfx_h_impat1,
    sfx_h_impat2,
    sfx_h_impdth,
    sfx_h_imppai,
    sfx_h_impsit,
    sfx_h_kgtat2,
    sfx_h_kgtatk,
    sfx_h_kgtdth,
    sfx_h_kgtpai,
    sfx_h_kgtsit,
    sfx_h_minact,
    sfx_h_minat1,
    sfx_h_minat2,
    sfx_h_minat3,
    sfx_h_mindth,
    sfx_h_minpai,
    sfx_h_minsit,
    sfx_h_mumat1,
    sfx_h_mumat2,
    sfx_h_mumdth,
    sfx_h_mumhed,
    sfx_h_mumpai,
    sfx_h_mumsit,
    sfx_h_snkact,
    sfx_h_snkatk,
    sfx_h_snkdth,
    sfx_h_snkpai,
    sfx_h_snksit,
    sfx_h_sorzap,
    sfx_h_sorsit,
    sfx_h_soratk,
    sfx_h_sorpai,
    sfx_h_soract,
    sfx_h_wizact,
    sfx_h_wizatk,
    sfx_h_wizdth,
    sfx_h_wizpai,
    sfx_h_wizsit,
    // ---- Hexen monster sounds (DS* lumps from hexenstuff.wad) ----
    sfx_x_etsit,
    sfx_x_etpai,
    sfx_x_etatk,
    sfx_x_etdth,
    sfx_x_cesit,
    sfx_x_ceact,
    sfx_x_cepai,
    sfx_x_ceatk,
    sfx_x_cedth,
    sfx_x_slatk,
    sfx_x_desit,
    sfx_x_depai,
    sfx_x_deatk,
    sfx_x_dedth,
    sfx_x_fdact,
    sfx_x_fdpai,
    sfx_x_fdatk,
    sfx_x_fddth,
    sfx_x_fdhit,
    sfx_x_wrsit,
    sfx_x_wract,
    sfx_x_wrpai,
    sfx_x_wratk,
    sfx_x_wrdth,
    sfx_x_bisit,
    sfx_x_biact,
    sfx_x_bipai,
    sfx_x_biatk,
    sfx_x_bidth,
    sfx_x_bihit,
    sfx_x_icsit,
    sfx_x_icatk,
    sfx_x_ichit,
    sfx_x_stsit,
    sfx_x_stact,
    sfx_x_stpai,
    sfx_x_statk,
    sfx_x_stdth,
    sfx_x_sthit,
    sfx_x_drsit,
    sfx_x_drpai,
    sfx_x_dratk,
    sfx_x_drdth,
    sfx_x_drhit,
    NUMSFX
} sfxenum_t;

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------

