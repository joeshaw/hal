
#include <stdio.h>
#include <stdlib.h>

int 
main (int argc, char *argv[])
{
	int i;

	fprintf (stderr, "hal-acl-remove %d\n", argc);
	for (i = 0; i < argc; i++) {
		fprintf (stderr, " arg %2d: %s\n", i, argv[i]);		
	}
	return 0;
}
