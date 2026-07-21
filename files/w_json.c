// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Minimal recursive-descent JSON parser -- see w_json.h.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "w_json.h"

typedef struct { const char* p; const char* end; boolean err; } jp_t;

static json_t* ParseValue (jp_t* s);

static void SkipWs (jp_t* s)
{
    while (s->p < s->end && isspace ((unsigned char)*s->p)) s->p++;
}

static json_t* NewNode (jsontype_t t)
{
    json_t* v = calloc (1, sizeof *v);
    if (v) v->type = t;
    return v;
}

// Parse a JSON string literal (leading '"' already at cursor).  Returns malloc'd.
static char* ParseStr (jp_t* s)
{
    char*  out;
    int    cap = 16, len = 0;
    if (*s->p != '"') { s->err = true; return NULL; }
    s->p++;
    out = malloc (cap);
    while (s->p < s->end && *s->p != '"')
    {
	char c = *s->p++;
	if (c == '\\' && s->p < s->end)
	{
	    char e = *s->p++;
	    switch (e)
	    {
		case 'n': c = '\n'; break;   case 't': c = '\t'; break;
		case 'r': c = '\r'; break;   case 'b': c = '\b'; break;
		case 'f': c = '\f'; break;   case '/': c = '/';  break;
		case '"': c = '"';  break;   case '\\': c = '\\'; break;
		case 'u': c = '?';  s->p += 4; break;	// \uXXXX -> placeholder
		default:  c = e; break;
	    }
	}
	if (len + 1 >= cap) { cap *= 2; out = realloc (out, cap); }
	out[len++] = c;
    }
    if (s->p < s->end) s->p++;			// closing quote
    else s->err = true;
    out[len] = 0;
    return out;
}

static json_t* ParseArray (jp_t* s)
{
    json_t* v = NewNode (JSON_ARR);
    int cap = 0;
    s->p++;					// '['
    SkipWs (s);
    if (s->p < s->end && *s->p == ']') { s->p++; return v; }
    while (s->p < s->end)
    {
	json_t* item = ParseValue (s);
	if (s->err) return v;
	if (v->n == cap) { cap = cap ? cap*2 : 8; v->items = realloc (v->items, cap*sizeof *v->items); }
	v->items[v->n++] = item;
	SkipWs (s);
	if (s->p < s->end && *s->p == ',') { s->p++; SkipWs (s); continue; }
	if (s->p < s->end && *s->p == ']') { s->p++; break; }
	s->err = true; break;
    }
    return v;
}

static json_t* ParseObject (jp_t* s)
{
    json_t* v = NewNode (JSON_OBJ);
    int cap = 0;
    s->p++;					// '{'
    SkipWs (s);
    if (s->p < s->end && *s->p == '}') { s->p++; return v; }
    while (s->p < s->end)
    {
	char* key;
	json_t* item;
	SkipWs (s);
	if (*s->p != '"') { s->err = true; break; }
	key = ParseStr (s);
	SkipWs (s);
	if (s->p >= s->end || *s->p != ':') { s->err = true; free (key); break; }
	s->p++;
	item = ParseValue (s);
	if (s->err) { free (key); return v; }
	if (v->n == cap) { cap = cap ? cap*2 : 8;
			   v->items = realloc (v->items, cap*sizeof *v->items);
			   v->keys  = realloc (v->keys,  cap*sizeof *v->keys); }
	v->keys[v->n]    = key;
	v->items[v->n++] = item;
	SkipWs (s);
	if (s->p < s->end && *s->p == ',') { s->p++; continue; }
	if (s->p < s->end && *s->p == '}') { s->p++; break; }
	s->err = true; break;
    }
    return v;
}

static json_t* ParseValue (jp_t* s)
{
    SkipWs (s);
    if (s->p >= s->end) { s->err = true; return NULL; }
    switch (*s->p)
    {
      case '{': return ParseObject (s);
      case '[': return ParseArray (s);
      case '"': { json_t* v = NewNode (JSON_STR); v->str = ParseStr (s); return v; }
      case 't': if (s->end - s->p >= 4 && !strncmp (s->p, "true", 4))  { s->p += 4; json_t* v = NewNode (JSON_BOOL); v->num = 1; return v; } break;
      case 'f': if (s->end - s->p >= 5 && !strncmp (s->p, "false", 5)) { s->p += 5; json_t* v = NewNode (JSON_BOOL); v->num = 0; return v; } break;
      case 'n': if (s->end - s->p >= 4 && !strncmp (s->p, "null", 4))  { s->p += 4; return NewNode (JSON_NULL); } break;
      default: break;
    }
    if (*s->p == '-' || isdigit ((unsigned char)*s->p))
    {
	char* endp;
	json_t* v = NewNode (JSON_NUM);
	v->num = strtod (s->p, &endp);
	if (endp == s->p) { s->err = true; }
	else s->p = endp;
	return v;
    }
    s->err = true;
    return NULL;
}

json_t* JSON_Parse (const char* text, int len)
{
    jp_t s; json_t* v;
    s.p = text; s.end = text + len; s.err = false;
    v = ParseValue (&s);
    if (s.err) { JSON_Free (v); return NULL; }
    return v;
}

void JSON_Free (json_t* v)
{
    int i;
    if (!v) return;
    if (v->str) free (v->str);
    for (i = 0; i < v->n; i++)
    {
	if (v->keys && v->keys[i]) free (v->keys[i]);
	JSON_Free (v->items[i]);
    }
    free (v->items); free (v->keys);
    free (v);
}

json_t* JSON_Get (const json_t* obj, const char* key)
{
    int i;
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (i = 0; i < obj->n; i++)
	if (obj->keys[i] && !strcmp (obj->keys[i], key)) return obj->items[i];
    return NULL;
}

json_t* JSON_Index (const json_t* arr, int i)
{
    if (!arr || arr->type != JSON_ARR || i < 0 || i >= arr->n) return NULL;
    return arr->items[i];
}

int JSON_Size (const json_t* v)		{ return v ? v->n : 0; }
const char* JSON_Str (const json_t* v)	{ return (v && v->type == JSON_STR && v->str) ? v->str : ""; }
double JSON_Num (const json_t* v, double def) { return (v && (v->type == JSON_NUM || v->type == JSON_BOOL)) ? v->num : def; }
boolean JSON_Bool (const json_t* v)	{ return v && ((v->type == JSON_BOOL && v->num != 0) || (v->type == JSON_NUM && v->num != 0)); }
