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
// $Log:$
//
// DESCRIPTION:
//	Handles WAD file header, directory, lump I/O.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: w_wad.c,v 1.5 1997/02/03 16:47:57 b1 Exp $";


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "m_swap.h"
#include "doomtype.h"
#include "i_system.h"
#include "z_zone.h"

#ifdef __GNUG__
#pragma implementation "w_wad.h"
#endif
#include "w_wad.h"
#include "miniz.h"






//
// GLOBALS
//

// Location of each lump on disk.
lumpinfo_t*		lumpinfo;		
int			numlumps;

void**			lumpcache;


#if defined(linux) || defined(__BEOS__) || defined(__SVR4)
void strupr (char* s)
{
    while (*s) { *s = toupper(*s); s++; }
}
#endif

int filelength (FILE *handle) 
{ 
    unsigned long pos, size;
    
    pos = ftell(handle);
printf("Position was %lu\n", pos);
    fseek(handle, 0, SEEK_END);
    size = ftell(handle);
    fseek(handle, pos, SEEK_SET);
printf("Size is %lu\n", size);

    return (int)size;
}


void
ExtractFileBase
( char*		path,
  char*		dest )
{
    char*	src;
    int		length;

    src = path + strlen(path) - 1;
    
    // back up until a \ or the start
    while (src != path
	   && *(src-1) != '\\'
	   && *(src-1) != '/')
    {
	src--;
    }
    
    // copy up to eight characters
    memset (dest,0,8);
    length = 0;
    
    while (*src && *src != '.')
    {
	if (length < 8)
	{
	    *dest++ = toupper((int)*src);
	    length++;
	}
	src++;
    }
}





//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// All files are optional, but at least one file must be
//  found (PWAD, if all required lumps are present).
// Files with a .wad extension are wadlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
//
// If filename starts with a tilde, the file is handled
//  specially to allow map reloads.
// But: the reload feature is a fragile hack...

#ifdef __BEOS__
#ifdef __GNUC__
extern void *alloca(int);
#else
#include <alloca.h>
#endif
#endif /* __BEOS__ */

#ifdef _WIN32
#include <malloc.h>		/* MSVC: alloca is the _alloca intrinsic */
#define alloca _alloca
#endif

int			reloadlump;
char*			reloadname;

#define MAX_ZIP_FILES 16
static mz_zip_archive open_zips[MAX_ZIP_FILES];
static int num_open_zips = 0;

void W_AddZipFile (char *filename)
{
    if (num_open_zips >= MAX_ZIP_FILES)
    {
        I_Error("W_AddZipFile: exceeded MAX_ZIP_FILES");
    }

    mz_zip_archive* zip = &open_zips[num_open_zips];
    memset(zip, 0, sizeof(*zip));

    FILE* handle = fopen(filename, "rb");
    if (handle == NULL)
    {
        // Game WADs live in run/ID0/ -- retry there before giving up, so bare names
        // resolve without an explicit path.
        static char id0path[1024];
        snprintf(id0path, sizeof(id0path), "ID0/%s", filename);
        handle = fopen(id0path, "rb");
        if (handle == NULL)
        {
            printf(" couldn't open zip/pk3 %s\n", filename);
            return;
        }
        filename = id0path;
    }
    fclose(handle);

    if (!mz_zip_reader_init_file(zip, filename, 0))
    {
        printf(" couldn't init zip reader for %s\n", filename);
        return;
    }

    printf(" adding zip %s\n", filename);
    int zip_idx = num_open_zips++;

    int num_files = mz_zip_reader_get_num_files(zip);

    int* sprite_indices = malloc(num_files * sizeof(int));
    int* flat_indices = malloc(num_files * sizeof(int));
    int* patch_indices = malloc(num_files * sizeof(int));
    int* map_indices = malloc(num_files * sizeof(int));
    int* global_indices = malloc(num_files * sizeof(int));

    int num_sprites = 0;
    int num_flats = 0;
    int num_patches = 0;
    int num_maps = 0;
    int num_globals = 0;

    for (int i = 0; i < num_files; i++)
    {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip, i, &file_stat))
            continue;

        if (mz_zip_reader_is_file_a_directory(zip, i))
            continue;

        char* path = file_stat.m_filename;
        if (strncasecmp(path, "sprites/", 8) == 0)
        {
            sprite_indices[num_sprites++] = i;
        }
        else if (strncasecmp(path, "flats/", 6) == 0)
        {
            flat_indices[num_flats++] = i;
        }
        else if (strncasecmp(path, "patches/", 8) == 0)
        {
            patch_indices[num_patches++] = i;
        }
        else if (strncasecmp(path, "maps/", 5) == 0 && strlen(path) > 9 && strcasecmp(path + strlen(path) - 4, ".wad") == 0)
        {
            map_indices[num_maps++] = i;
        }
        else
        {
            if (strstr(path, "__MACOSX") || strstr(path, ".DS_Store"))
                continue;
            global_indices[num_globals++] = i;
        }
    }

    int added_lumps = num_globals;
    if (num_sprites > 0) added_lumps += num_sprites + 2;
    if (num_flats > 0)    added_lumps += num_flats + 2;
    if (num_patches > 0)  added_lumps += num_patches + 2;

    int startlump = numlumps;
    numlumps += added_lumps;
    lumpinfo = realloc(lumpinfo, numlumps * sizeof(lumpinfo_t));
    if (!lumpinfo)
        I_Error("Couldn't realloc lumpinfo for zip");

    lumpinfo_t* lump_p = &lumpinfo[startlump];

    #define SET_ZIP_LUMP(name_str, f_idx) \
        lump_p->handle = NULL; \
        lump_p->position = 0; \
        lump_p->size = 0; \
        lump_p->zip_idx = zip_idx; \
        lump_p->zip_file_index = f_idx; \
        lump_p->mem_data = NULL; \
        if (f_idx >= 0) { \
            mz_zip_archive_file_stat st; \
            mz_zip_reader_file_stat(zip, f_idx, &st); \
            lump_p->size = (int)st.m_uncomp_size; \
        } \
        memset(lump_p->name, 0, 8); \
        strncpy(lump_p->name, name_str, 8); \
        lump_p++;

    // 1. Add globals
    for (int i = 0; i < num_globals; i++)
    {
        mz_zip_archive_file_stat file_stat;
        mz_zip_reader_file_stat(zip, global_indices[i], &file_stat);
        char base_name[9];
        ExtractFileBase(file_stat.m_filename, base_name);
        SET_ZIP_LUMP(base_name, global_indices[i]);
    }

    // 2. Add sprites
    if (num_sprites > 0)
    {
        SET_ZIP_LUMP("S_START", -1);
        for (int i = 0; i < num_sprites; i++)
        {
            mz_zip_archive_file_stat file_stat;
            mz_zip_reader_file_stat(zip, sprite_indices[i], &file_stat);
            char base_name[9];
            ExtractFileBase(file_stat.m_filename, base_name);
            SET_ZIP_LUMP(base_name, sprite_indices[i]);
        }
        SET_ZIP_LUMP("S_END", -1);
    }

    // 3. Add flats
    if (num_flats > 0)
    {
        SET_ZIP_LUMP("F_START", -1);
        for (int i = 0; i < num_flats; i++)
        {
            mz_zip_archive_file_stat file_stat;
            mz_zip_reader_file_stat(zip, flat_indices[i], &file_stat);
            char base_name[9];
            ExtractFileBase(file_stat.m_filename, base_name);
            SET_ZIP_LUMP(base_name, flat_indices[i]);
        }
        SET_ZIP_LUMP("F_END", -1);
    }

    // 4. Add patches
    if (num_patches > 0)
    {
        SET_ZIP_LUMP("P_START", -1);
        for (int i = 0; i < num_patches; i++)
        {
            mz_zip_archive_file_stat file_stat;
            mz_zip_reader_file_stat(zip, patch_indices[i], &file_stat);
            char base_name[9];
            ExtractFileBase(file_stat.m_filename, base_name);
            SET_ZIP_LUMP(base_name, patch_indices[i]);
        }
        SET_ZIP_LUMP("P_END", -1);
    }

    #undef SET_ZIP_LUMP

    free(sprite_indices);
    free(flat_indices);
    free(patch_indices);
    free(global_indices);

    // 5. Load map WADs
    for (int i = 0; i < num_maps; i++)
    {
        size_t wad_size;
        void* wad_data = mz_zip_reader_extract_to_heap(zip, map_indices[i], &wad_size, 0);
        if (!wad_data)
        {
            printf(" failed to extract map wad %d\n", map_indices[i]);
            continue;
        }

        wadinfo_t* header = (wadinfo_t*)wad_data;
        if (strncmp(header->identification, "IWAD", 4) != 0 && strncmp(header->identification, "PWAD", 4) != 0)
        {
            printf(" embedded wad in zip doesn't have IWAD or PWAD id\n");
            free(wad_data);
            continue;
        }

        int map_numlumps = LONG(header->numlumps);
        int infotableofs = LONG(header->infotableofs);
        filelump_t* fileinfo = (filelump_t*)((byte*)wad_data + infotableofs);

        int map_startlump = numlumps;
        numlumps += map_numlumps;
        lumpinfo = realloc(lumpinfo, numlumps * sizeof(lumpinfo_t));
        if (!lumpinfo)
            I_Error("Couldn't realloc lumpinfo for map in zip");

        lumpinfo_t* map_lump_p = &lumpinfo[map_startlump];
        for (int j = 0; j < map_numlumps; j++, map_lump_p++, fileinfo++)
        {
            map_lump_p->handle = NULL;
            map_lump_p->position = LONG(fileinfo->filepos);
            map_lump_p->size = LONG(fileinfo->size);
            map_lump_p->zip_idx = -1;
            map_lump_p->zip_file_index = -1;
            map_lump_p->mem_data = (byte*)wad_data;
            strncpy(map_lump_p->name, fileinfo->name, 8);
        }
    }

    free(map_indices);
}


void W_AddFile (char *filename)
{
    wadinfo_t		header;
    lumpinfo_t*		lump_p;
    unsigned		i;
    FILE	       *handle;
    int			length;
    int			startlump;
    filelump_t*		fileinfo;
    filelump_t		singleinfo;
    FILE		*storehandle;
    
    // open the file and add to directory

    // handle reload indicator.
    if (filename[0] == '~')
    {
	filename++;
	reloadname = filename;
	reloadlump = numlumps;
    }
		
    int fn_len = strlen(filename);
    if (fn_len >= 4 && (I_strncasecmp(filename + fn_len - 3, "pk3", 3) == 0 || I_strncasecmp(filename + fn_len - 3, "zip", 3) == 0))
    {
        W_AddZipFile(filename);
        return;
    }

    if ( (handle = fopen (filename,"rb")) == NULL)
    {
	// Game WADs live in run/ID0/ -- retry there before giving up, so bare names
	// (aidoom.wad, -file doom2stuff.wad, ...) resolve without an explicit path.
	static char id0path[1024];
	snprintf (id0path, sizeof(id0path), "ID0/%s", filename);
	if ( (handle = fopen (id0path,"rb")) == NULL)
	{
	    printf (" couldn't open %s\n",filename);
	    return;
	}
	filename = id0path;
    }

    printf (" adding %s\n",filename);
    startlump = numlumps;
	
    if (I_strncasecmp (filename+strlen(filename)-3 , "wad", 3 ) )
    {
	// single lump file
	fileinfo = &singleinfo;
	singleinfo.filepos = 0;
	singleinfo.size = LONG(filelength(handle));
	ExtractFileBase (filename, singleinfo.name);
	numlumps++;
    }
    else 
    {
	// WAD file
	fread (&header, 1, sizeof(header), handle);
	if (strncmp(header.identification,"IWAD",4))
	{
	    // Homebrew levels?
	    if (strncmp(header.identification,"PWAD",4))
	    {
		I_Error ("Wad file %s doesn't have IWAD "
			 "or PWAD id\n", filename);
	    }
	    
	    // ???modifiedgame = true;		
	}
	header.numlumps = LONG(header.numlumps);
	header.infotableofs = LONG(header.infotableofs);
	length = header.numlumps*sizeof(filelump_t);
	fileinfo = alloca (length);
	fseek (handle, header.infotableofs, SEEK_SET);
	fread (fileinfo, 1, length, handle);
	numlumps += header.numlumps;
    }

    
    // Fill in lumpinfo
    lumpinfo = realloc (lumpinfo, numlumps*sizeof(lumpinfo_t));

    if (!lumpinfo)
	I_Error ("Couldn't realloc lumpinfo");

    lump_p = &lumpinfo[startlump];
	
    storehandle = reloadname ? NULL : handle;
	
    for (i=startlump ; i<numlumps ; i++,lump_p++, fileinfo++)
    {
	lump_p->handle = storehandle;
	lump_p->position = LONG(fileinfo->filepos);
	lump_p->size = LONG(fileinfo->size);
	lump_p->zip_idx = -1;
	lump_p->zip_file_index = -1;
	lump_p->mem_data = NULL;
	strncpy (lump_p->name, fileinfo->name, 8);
    }
	
    if (reloadname)
	fclose (handle);
}




//
// W_Reload
// Flushes any of the reloadable lumps in memory
//  and reloads the directory.
//
void W_Reload (void)
{
    wadinfo_t		header;
    int			lumpcount;
    lumpinfo_t*		lump_p;
    unsigned		i;
    FILE		*handle;
    int			length;
    filelump_t*		fileinfo;
	
    if (!reloadname)
	return;
		
    if ( (handle = fopen (reloadname,"rb")) == NULL)
	I_Error ("W_Reload: couldn't open %s",reloadname);

    fread (&header, 1, sizeof(header), handle);
    lumpcount = LONG(header.numlumps);
    header.infotableofs = LONG(header.infotableofs);
    length = lumpcount*sizeof(filelump_t);
    fileinfo = alloca (length);
    fseek (handle, header.infotableofs, SEEK_SET);
    fread (fileinfo, 1, length, handle);
    
    // Fill in lumpinfo
    lump_p = &lumpinfo[reloadlump];
	
    for (i=reloadlump ;
	 i<reloadlump+lumpcount ;
	 i++,lump_p++, fileinfo++)
    {
	if (lumpcache[i])
	    Z_Free (lumpcache[i]);

	lump_p->position = LONG(fileinfo->filepos);
	lump_p->size = LONG(fileinfo->size);
    }
	
    fclose (handle);
}



//
// W_InitMultipleFiles
// Pass a null terminated list of files to use.
// All files are optional, but at least one file
//  must be found.
// Files with a .wad extension are idlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
// Lump names can appear multiple times.
// The name searcher looks backwards, so a later file
//  does override all earlier ones.
//
void W_InitMultipleFiles (char** filenames)
{	
    int		size;
    
    // open all the files, load headers, and count lumps
    numlumps = 0;

    // will be realloced as lumps are added
    lumpinfo = malloc(1);	

    for ( ; *filenames ; filenames++)
	W_AddFile (*filenames);

    if (!numlumps)
	I_Error ("W_InitFiles: no files found");
    
    // set up caching
    size = numlumps * sizeof(*lumpcache);
    lumpcache = malloc (size);
    
    if (!lumpcache)
	I_Error ("Couldn't allocate lumpcache");

    memset (lumpcache,0, size);
}




//
// W_InitFile
// Just initialize from a single file.
//
void W_InitFile (char* filename)
{
    char*	names[2];

    names[0] = filename;
    names[1] = NULL;
    W_InitMultipleFiles (names);
}



//
// W_NumLumps
//
int W_NumLumps (void)
{
    return numlumps;
}



//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (char* name)
{
    union {
	char	s[9];
	int	x[2];
	
    } name8;
    
    int		v1;
    int		v2;
    lumpinfo_t*	lump_p;

    // make the name into two integers for easy compares
    strncpy (name8.s,name,8);

    // in case the name was a fill 8 chars
    name8.s[8] = 0;

    // case insensitive
    strupr (name8.s);		

    v1 = name8.x[0];
    v2 = name8.x[1];


    // scan backwards so patch lump files take precedence
    lump_p = lumpinfo + numlumps;

    while (lump_p-- != lumpinfo)
    {
	if ( *(int *)lump_p->name == v1
	     && *(int *)&lump_p->name[4] == v2)
	{
	    return lump_p - lumpinfo;
	}
    }

    // TFB. Not found.
    return -1;
}




//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (char* name)
{
    int	i;

    i = W_CheckNumForName (name);
    
    if (i == -1)
      I_Error ("W_GetNumForName: %s not found!", name);
      
    return i;
}


//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (int lump)
{
    if (lump >= numlumps)
	I_Error ("W_LumpLength: %i >= numlumps",lump);

    return lumpinfo[lump].size;
}



void
W_ReadLump
( int		lump,
  void*		dest )
{
    int		c;
    lumpinfo_t*	l;
    FILE	*handle;
	
    if (lump >= numlumps)
	I_Error ("W_ReadLump: %i >= numlumps",lump);

    l = lumpinfo+lump;
	
    if (l->mem_data != NULL)
    {
        memcpy (dest, l->mem_data + l->position, l->size);
        return;
    }

    if (l->zip_idx >= 0)
    {
        if (l->zip_file_index >= 0)
        {
            mz_bool success = mz_zip_reader_extract_to_mem_no_alloc(
                &open_zips[l->zip_idx],
                l->zip_file_index,
                dest,
                l->size,
                0,
                NULL,
                0
            );
            if (!success)
                I_Error("W_ReadLump: failed to extract zip lump %d", lump);
        }
        return;
    }

    // ??? I_BeginRead ();
	
    if (l->handle == NULL)
    {
	// reloadable file, so use open / read / close
	if ( (handle = fopen (reloadname,"rb")) == NULL)
	    I_Error ("W_ReadLump: couldn't open %s",reloadname);
    }
    else
	handle = l->handle;
		
    fseek (handle, l->position, SEEK_SET);
    c = fread (dest, 1, l->size, handle);

    if (c < l->size)
	I_Error ("W_ReadLump: only read %i of %i on lump %i",
		 c,l->size,lump);	

    if (l->handle == NULL)
	fclose (handle);
		
    // ??? I_EndRead ();
}




//
// W_CacheLumpNum
//
void*
W_CacheLumpNum
( int		lump,
  int		tag )
{
    byte*	ptr;

    if ((unsigned)lump >= numlumps)
	I_Error ("W_CacheLumpNum: %i >= numlumps",lump);
		
    if (!lumpcache[lump])
    {
	// read the lump in
	
	//printf ("cache miss on lump %i\n",lump);
	ptr = Z_Malloc (W_LumpLength (lump), tag, &lumpcache[lump]);
	W_ReadLump (lump, lumpcache[lump]);
    }
    else
    {
	//printf ("cache hit on lump %i\n",lump);
	Z_ChangeTag (lumpcache[lump],tag);
    }
	
    return lumpcache[lump];
}



//
// W_CacheLumpName
//
void*
W_CacheLumpName
( char*		name,
  int		tag )
{
    return W_CacheLumpNum (W_GetNumForName(name), tag);
}


//
// W_Profile
//
int		info[2500][10];
int		profilecount;

void W_Profile (void)
{
    int		i;
    memblock_t*	block;
    void*	ptr;
    char	ch;
    FILE*	f;
    int		j;
    char	name[9];
	
	
    for (i=0 ; i<numlumps ; i++)
    {	
	ptr = lumpcache[i];
	if (!ptr)
	{
	    ch = ' ';
	    continue;
	}
	else
	{
	    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	    if (block->tag < PU_PURGELEVEL)
		ch = 'S';
	    else
		ch = 'P';
	}
	info[i][profilecount] = ch;
    }
    profilecount++;
	
    f = fopen ("waddump.txt","w");
    name[8] = 0;

    for (i=0 ; i<numlumps ; i++)
    {
	memcpy (name,lumpinfo[i].name,8);

	for (j=0 ; j<8 ; j++)
	    if (!name[j])
		break;

	for ( ; j<8 ; j++)
	    name[j] = ' ';

	fprintf (f,"%s ",name);

	for (j=0 ; j<profilecount ; j++)
	    fprintf (f,"    %c",info[i][j]);

	fprintf (f,"\n");
    }
    fclose (f);
}


