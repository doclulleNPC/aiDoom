// dsdhacked.c -- DSDHacked (M3): grow the state / mobjinfo tables on demand so DeHackEd patches
// can reference frame/thing numbers far beyond the built-in set (DECOHack / MBF21 patches).
// The tables start as the built-in arrays (see info.c); the first growth malloc+copies them.
#include <stdlib.h>
#include <string.h>
#include "doomdef.h"
#include "info.h"

actionf_t *deh_codeptr = NULL;   // per-state original action (grown with states; used by d_deh.c)

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
