// ID24 Legacy-of-Rust content installer -- see files/id24.c.
#ifndef __ID24_H__
#define __ID24_H__

void	ID24_Init (void);		// install sprites/sounds/states/things (before R_Init)
int	ID24_Available (void);		// true once the LoR content is installed
int	ID24_TypeByName (const char* s);// console spawn: monster name -> mobjtype, or -1
int	ID24_Give (void* player, const char* s);	// console give: weapon/fuel, 1 if handled

#endif
