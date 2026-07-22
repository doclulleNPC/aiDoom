// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	ID24 SBARDEF -- data-driven status bar (https://doomwiki.org/wiki/SBARDEF).
//	Parses the SBARDEF JSON lump (id24res.wad) into number fonts + status-bar
//	element trees and draws them.  Covers the element/number/condition subset the
//	Legacy-of-Rust bar uses (graphic, face, facebackground, number, percent,
//	canvas; number types 0-5; the conditions in that lump).  Opt-in with -sbardef
//	(or the `use_sbardef` config) so default play is unaffected.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "m_swap.h"
#include "w_wad.h"
#include "z_zone.h"
#include "v_video.h"
#include "w_json.h"
#include "d_items.h"
#include "d_player.h"
#include "st_sbardef.h"

extern weaponinfo_t	weaponinfo[];
extern patch_t*		ST_SBFace (void);
extern patch_t*		ST_SBFaceBack (void);

// element types (index of the naming key in the object)
enum { SBE_GRAPHIC, SBE_ANIM, SBE_FACE, SBE_FACEBG, SBE_NUMBER, SBE_PERCENT, SBE_CANVAS, SBE_UNKNOWN };

typedef struct { int cond, param; } cond_t;

typedef struct sbe_s {
    int		type;
    int		x, y, align;
    char	patch[9];		// graphic
    int		numtype, param, maxlen;	// number/percent
    int		font;			// index into fonts[]
    cond_t*	conds;	int nconds;
    struct sbe_s* kids; int nkids;
} sbe_t;

typedef struct { char name[40]; patch_t* d[10]; patch_t* pct; patch_t* neg; int w; } sbfont_t;
typedef struct { int height; boolean fullscreen; sbe_t* kids; int nkids; } sbar_t;

static sbfont_t*	fonts;   static int nfonts;
static sbar_t*		bars;    static int nbars;
static int		use_sbardef = 0;	// -sbardef / config
int			use_sbardef_cfg = 0;	// exported for m_misc defaults

static patch_t* CachePatchSafe (const char* nm)
{
    if (!nm || !nm[0] || W_CheckNumForName ((char*)nm) < 0) return NULL;
    return (patch_t*) W_CacheLumpName ((char*)nm, PU_STATIC);
}

// ------------------------------------------------------------------ parse
static int ElemType (const json_t* o, json_t** inner)
{
    static const char* keys[] = {"graphic","animation","face","facebackground","number","percent","canvas"};
    int i;
    for (i = 0; i < 7; i++) { json_t* v = JSON_Get (o, keys[i]); if (v) { *inner = v; return i; } }
    return SBE_UNKNOWN;
}

static void ParseElems (json_t* arr, sbe_t** out, int* n);

static void ParseElem (json_t* o, sbe_t* e)
{
    json_t* in = NULL;
    json_t* c;
    memset (e, 0, sizeof *e);
    e->type = ElemType (o, &in);
    if (!in) return;
    e->x     = (int) JSON_Num (JSON_Get (in, "x"), 0);
    e->y     = (int) JSON_Num (JSON_Get (in, "y"), 0);
    e->align = (int) JSON_Num (JSON_Get (in, "alignment"), 0);
    { const char* p = JSON_Str (JSON_Get (in, "patch")); int k; for (k=0;k<8&&p[k];k++) e->patch[k]=p[k]; }
    e->numtype = (int) JSON_Num (JSON_Get (in, "type"), 0);
    e->param   = (int) JSON_Num (JSON_Get (in, "param"), 0);
    e->maxlen  = (int) JSON_Num (JSON_Get (in, "maxlength"), 3);
    e->font    = -1;
    { const char* fn = JSON_Str (JSON_Get (in, "font")); int i;
      for (i = 0; i < nfonts; i++) if (!strcmp (fn, fonts[i].name)) { e->font = i; break; } }
    // conditions
    c = JSON_Get (in, "conditions");
    if (c && c->type == JSON_ARR && c->n > 0)
    {
	int i; e->conds = malloc (c->n * sizeof(cond_t)); e->nconds = c->n;
	for (i = 0; i < c->n; i++)
	{
	    json_t* cc = JSON_Index (c, i);
	    e->conds[i].cond  = (int) JSON_Num (JSON_Get (cc, "condition"), -1);
	    e->conds[i].param = (int) JSON_Num (JSON_Get (cc, "param"), 0);
	}
    }
    ParseElems (JSON_Get (in, "children"), &e->kids, &e->nkids);
}

static void ParseElems (json_t* arr, sbe_t** out, int* n)
{
    int i;
    *out = NULL; *n = 0;
    if (!arr || arr->type != JSON_ARR || arr->n == 0) return;
    *out = malloc (arr->n * sizeof(sbe_t)); *n = arr->n;
    for (i = 0; i < arr->n; i++) ParseElem (JSON_Index (arr, i), &(*out)[i]);
}

void ST_SBARDEF_Init (void)
{
    int lump = W_CheckNumForName ("SBARDEF");
    json_t *root, *data, *nf, *sb;
    int i;

    // Auto-enable when actually playing an ID24 set (GAMECONF `executable: id24`,
    // i.e. Legacy of Rust) so its custom bar is used without a flag; -sbardef / the
    // config force it on for other WADs that ship a SBARDEF.
    { extern int gameconf_id24;
      use_sbardef = (M_CheckParm ("-sbardef") > 0) || use_sbardef_cfg || gameconf_id24; }
    if (lump < 0) return;

    root = JSON_Parse ((const char*) W_CacheLumpNum (lump, PU_CACHE), W_LumpLength (lump));
    if (!root) { fprintf (stderr, "SBARDEF: JSON parse error -- ignored.\n"); return; }
    data = JSON_Get (root, "data");

    // number fonts: load stem+"NUM0..9", stem+"PRCNT", stem+"MINUS"
    nf = data ? JSON_Get (data, "numberfonts") : NULL;
    if (nf && nf->type == JSON_ARR)
    {
	fonts = malloc (nf->n * sizeof(sbfont_t)); nfonts = nf->n;
	for (i = 0; i < nf->n; i++)
	{
	    json_t* f = JSON_Index (nf, i);
	    const char* nm = JSON_Str (JSON_Get (f, "name"));
	    const char* stem = JSON_Str (JSON_Get (f, "stem"));
	    char b[16]; int d;
	    memset (&fonts[i], 0, sizeof(sbfont_t));
	    snprintf (fonts[i].name, sizeof fonts[i].name, "%s", nm);
	    for (d = 0; d < 10; d++) { snprintf (b, sizeof b, "%.5sNUM%d", stem, d); fonts[i].d[d] = CachePatchSafe (b); }
	    snprintf (b, sizeof b, "%.5sPRCNT", stem); fonts[i].pct = CachePatchSafe (b);
	    snprintf (b, sizeof b, "%.5sMINUS", stem); fonts[i].neg = CachePatchSafe (b);
	    fonts[i].w = (fonts[i].d[0]) ? SHORT (fonts[i].d[0]->width) : 0;
	}
    }

    // status bars
    sb = data ? JSON_Get (data, "statusbars") : NULL;
    if (sb && sb->type == JSON_ARR)
    {
	bars = malloc (sb->n * sizeof(sbar_t)); nbars = sb->n;
	for (i = 0; i < sb->n; i++)
	{
	    json_t* s = JSON_Index (sb, i);
	    bars[i].height     = (int) JSON_Num (JSON_Get (s, "height"), 32);
	    bars[i].fullscreen = JSON_Bool (JSON_Get (s, "fullscreenrender"));
	    ParseElems (JSON_Get (s, "children"), &bars[i].kids, &bars[i].nkids);
	}
    }

    JSON_Free (root);
    printf ("SBARDEF: %d bar(s), %d number font(s)%s.\n", nbars, nfonts,
	    use_sbardef ? " (active)" : " (parsed; enable with -sbardef)");
}

int ST_SBARDEF_Active (void) { return use_sbardef && nbars > 0; }

// ------------------------------------------------------------------ eval
static boolean SlotOwned (player_t* p, int slot)
{
    switch (slot) {
      case 1: return p->weaponowned[wp_fist] || p->weaponowned[wp_chainsaw];
      case 2: return p->weaponowned[wp_pistol];
      case 3: return p->weaponowned[wp_shotgun] || p->weaponowned[wp_supershotgun];
      case 4: return p->weaponowned[wp_chaingun];
      case 5: return p->weaponowned[wp_missile];
      case 6: return p->weaponowned[wp_plasma];
      case 7: return p->weaponowned[wp_bfg];
    }
    return false;
}

static int NumVal (player_t* p, int t, int param)
{
    ammotype_t a = weaponinfo[p->readyweapon].ammo;
    int f, i;
    switch (t) {
      case 0: return p->health;
      case 1: return p->armorpoints;
      case 2: f = 0; for (i=0;i<MAXPLAYERS;i++) f += p->frags[i]; return f;
      // param is an ammo-type index (0..NUMAMMO-1).  Bound-check UNSIGNED so a
      // negative/garbage param (id24res.wad's bar ships a 0x90000000 sentinel)
      // can't index p->ammo[] out of bounds -> the crash this replaced.
      case 3: return ((unsigned)param < NUMAMMO) ? p->ammo[param] : 0;	// ammo of type `param`
      case 4: return ((unsigned)a     < NUMAMMO) ? p->ammo[a]     : 0;	// selected weapon's ammo
      case 5: return ((unsigned)param < NUMAMMO) ? p->maxammo[param] : 0;	// maxammo of type `param`
    }
    return 0;
}

static boolean CondOK (player_t* p, int c, int param)
{
    ammotype_t a = weaponinfo[p->readyweapon].ammo;
    int session = deathmatch ? 2 : (netgame ? 1 : 0);
    switch (c) {
      case 4:  return a < NUMAMMO;			// selectedweaponhasammo
      case 5:  return (int) a == param;			// selectedweaponammotype
      case 6:  return SlotOwned (p, param);		// weaponslotowned
      case 7:  return !SlotOwned (p, param);		// weaponslotnotowned
      case 10: return false;				// itemowned (DOOM has none tracked)
      case 11: return true;				// itemnotowned
      case 12: return true;				// featurelevel >= (we are id24)
      case 13: return false;				// featurelevel <
      case 14: return session == param;			// sessiontypeequal
      case 15: return session != param;			// sessiontypenotequal
      case 18: return true;				// hudmodeequal (draw regardless)
    }
    return true;
}

// ------------------------------------------------------------------ draw
static void DrawNum (sbfont_t* f, int x, int y, int val, int align, boolean pct)
{
    char buf[16]; int i, len, w, dx;
    boolean neg = val < 0;
    if (!f || !f->d[0]) return;
    if (neg) val = -val;
    len = snprintf (buf, sizeof buf, "%d", val);
    w = len * f->w + (neg && f->neg ? f->w : 0) + (pct && f->pct ? SHORT(f->pct->width) : 0);
    dx = x;
    if ((align & 3) == 2) dx -= w;			// h_right: x is the right edge
    else if ((align & 3) == 1) dx -= w/2;		// h_middle
    if (neg && f->neg) { V_DrawPatch (dx, y, 0, f->neg); dx += f->w; }
    for (i = 0; i < len; i++) { int d = buf[i]-'0'; if (f->d[d]) V_DrawPatch (dx, y, 0, f->d[d]); dx += f->w; }
    if (pct && f->pct) V_DrawPatch (dx, y, 0, f->pct);
}

static void DrawElem (player_t* p, sbe_t* e, int ox, int oy)
{
    int i, x, y;
    for (i = 0; i < e->nconds; i++)
	if (!CondOK (p, e->conds[i].cond, e->conds[i].param)) return;

    // NB: WIDESCREENDELTA is applied ONCE by the caller (ST_SBARDEF_Draw) as the
    // initial ox, not here -- adding it per element would multiply it by the
    // nesting depth and drift nested widgets rightward in widescreen.
    x = ox + e->x;
    y = oy + e->y;

    switch (e->type) {
      case SBE_GRAPHIC: {
	patch_t* g = CachePatchSafe (e->patch);
	if (g) { int dx=x, dy=y;
	    if ((e->align&3)==2) dx -= SHORT(g->width); else if ((e->align&3)==1) dx -= SHORT(g->width)/2;
	    if ((e->align&0xC)==8) dy -= SHORT(g->height); else if ((e->align&0xC)==4) dy -= SHORT(g->height)/2;
	    V_DrawPatch (dx, dy, 0, g); }
	break; }
      case SBE_FACEBG: { patch_t* g = ST_SBFaceBack (); if (g) V_DrawPatch (x, y, 0, g); break; }
      case SBE_FACE:   { patch_t* g = ST_SBFace ();     if (g) V_DrawPatch (x, y, 0, g); break; }
      case SBE_NUMBER: if (e->font>=0) DrawNum (&fonts[e->font], x, y, NumVal (p, e->numtype, e->param), e->align, false); break;
      case SBE_PERCENT:if (e->font>=0) DrawNum (&fonts[e->font], x, y, NumVal (p, e->numtype, e->param), e->align, true);  break;
      case SBE_CANVAS: break;	// container -- just offsets its children
      default: break;
    }
    for (i = 0; i < e->nkids; i++) DrawElem (p, &e->kids[i], x, y);
}

void ST_SBARDEF_Draw (boolean fullscreen)
{
    player_t* p = &players[displayplayer];
    sbar_t* bar = NULL;
    int i, basey;

    // pick a bar: the classic (non-fullscreen) one unless in fullscreen HUD mode.
    for (i = 0; i < nbars; i++)
    {
	if (fullscreen && !automapactive) { if (bars[i].fullscreen)  { bar = &bars[i]; break; } }
	else                              { if (!bars[i].fullscreen) { bar = &bars[i]; break; } }
    }
    if (!bar) return;
    basey = bar->fullscreen ? 0 : (BASE_HEIGHT - bar->height);	// classic bar sits at the bottom
    // WIDESCREENDELTA applied once here as the root x-origin (see DrawElem).
    for (i = 0; i < bar->nkids; i++) DrawElem (p, &bar->kids[i], WIDESCREENDELTA, basey);
}
