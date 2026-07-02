// dsdhacked.c -- DSDHacked (M3): grow the state / mobjinfo tables on demand so DeHackEd patches
// can reference frame/thing numbers far beyond the built-in set (DECOHack / MBF21 patches).
// The tables start as the built-in arrays (see info.c); the first growth malloc+copies them.
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "info.h"
#include "sounds.h"
extern int *lengths;

actionf_t *deh_codeptr = NULL;   // per-state original action (grown with states; used by d_deh.c)
int *seenstate_tab = NULL;       // P_SetMobjState cycle-detection table (grown with states)

void dsdh_EnsureStatesCapacity (int limit)
{
    int old, newn, i;
    state_t *ns;
    if (limit < num_states) return;
    old = num_states; newn = limit + 1;
    if (states == states_builtin)
        ns = memcpy (malloc (newn * sizeof(state_t)), states_builtin, old * sizeof(state_t));
    else
        ns = realloc (states, newn * sizeof(state_t));
    memset (ns + old, 0, (newn - old) * sizeof(state_t));
    for (i = old; i < newn; i++) { ns[i].tics = -1; ns[i].nextstate = i; }  // idle until DEH'd
    states = ns;
    deh_codeptr = realloc (deh_codeptr, newn * sizeof(actionf_t));
    memset (deh_codeptr + old, 0, (newn - old) * sizeof(actionf_t));
    seenstate_tab = realloc (seenstate_tab, newn * sizeof(int));
    memset (seenstate_tab + old, 0, (newn - old) * sizeof(int));
    num_states = newn;
}

void dsdh_EnsureMobjInfoCapacity (int limit)
{
    int old, newn;
    mobjinfo_t *nm;
    if (limit < num_mobjtypes) return;
    old = num_mobjtypes; newn = limit + 1;
    if (mobjinfo == mobjinfo_builtin)
        nm = memcpy (malloc (newn * sizeof(mobjinfo_t)), mobjinfo_builtin, old * sizeof(mobjinfo_t));
    else
        nm = realloc (mobjinfo, newn * sizeof(mobjinfo_t));
    memset (nm + old, 0, (newn - old) * sizeof(mobjinfo_t));
    mobjinfo = nm;
    num_mobjtypes = newn;
}

static char sprname_empty[4] = { 0, 0, 0, 0 };   // reads as int 0 -> matches no lump

void dsdh_EnsureSpritesCapacity (int limit)
{
    int old, newn, i;
    char **np;
    if (limit < num_sprites) return;
    old = num_sprites; newn = limit + 1;
    if (sprnames == sprnames_builtin)
        np = memcpy (malloc (newn * sizeof(char*)), sprnames_builtin, old * sizeof(char*));
    else
        np = realloc (sprnames, newn * sizeof(char*));
    for (i = old; i < newn; i++) np[i] = sprname_empty;   // gap slots -> non-matching
    sprnames = np;
    num_sprites = newn;
}

void dsdh_EnsureSFXCapacity (int limit)
{
    int old, newn, i;
    sfxinfo_t *ns; int *nl;
    if (limit < num_sfx) return;
    old = num_sfx; newn = limit + 1;
    if (S_sfx == S_sfx_builtin)
        ns = memcpy (malloc (newn * sizeof(sfxinfo_t)), S_sfx_builtin, old * sizeof(sfxinfo_t));
    else
        ns = realloc (S_sfx, newn * sizeof(sfxinfo_t));
    memset (ns + old, 0, (newn - old) * sizeof(sfxinfo_t));
    // rebase any intra-table link pointer (only chgun->pistol) into the new array
    for (i = 0; i < old; i++)
        if (ns[i].link) ns[i].link = ns + (ns[i].link - S_sfx_builtin);
    for (i = old; i < newn; i++) { ns[i].lumpnum = -1; ns[i].usefulness = -1; ns[i].priority = 64; }
    S_sfx = ns;
    if (lengths) { nl = realloc (lengths, newn * sizeof(int)); memset (nl+old, 0, (newn-old)*sizeof(int)); lengths = nl; }
    num_sfx = newn;
}
