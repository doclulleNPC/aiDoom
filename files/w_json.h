// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Minimal JSON parser for the ID24 "JSON Lump" formats (SKYDEFS, GAMECONF,
//	DEMOLOOP, SBARDEF).  Parses into a small node tree; enough of JSON for those
//	lumps (objects, arrays, strings, numbers, true/false/null).  Not a general
//	streaming/Unicode-perfect parser -- \uXXXX escapes are passed through as-is.
//
//-----------------------------------------------------------------------------
#ifndef __W_JSON__
#define __W_JSON__

#include "doomtype.h"

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } jsontype_t;

typedef struct json_s
{
    jsontype_t		type;
    double		num;	// JSON_NUM; JSON_BOOL uses 0/1
    char*		str;	// JSON_STR value (malloc'd)
    struct json_s**	items;	// JSON_ARR / JSON_OBJ values (malloc'd array)
    char**		keys;	// JSON_OBJ member names (parallel to items)
    int			n;	// item count
} json_t;

json_t*		JSON_Parse (const char* text, int len);	// NULL on syntax error
void		JSON_Free  (json_t* v);

json_t*		JSON_Get   (const json_t* obj, const char* key);	// object member or NULL
json_t*		JSON_Index (const json_t* arr, int i);			// array element or NULL
int		JSON_Size  (const json_t* v);				// array/object length
const char*	JSON_Str   (const json_t* v);				// "" if not a string
double		JSON_Num   (const json_t* v, double def);		// def if not a number
boolean		JSON_Bool  (const json_t* v);

#endif	// __W_JSON__
