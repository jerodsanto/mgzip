#ifndef MGZIP_H
#define MGZIP_H

#include "config.h"

#define MAX_THREADS 32  /* abitrary - for array allocation.  */
#define DEFAULT_COMPRESSION_LEVEL 2
#define DEFAULT_THREADS 2
#define CHUNK_SIZE 131072
#define DEF_MEM_LEVEL 8

typedef struct { 
	int force_stdout;
	int preserve_infile;
	int compress_level;
	int num_threads; 
	int chunk_size;
	FILE *infile; 
	FILE *outfile;
	int num_files;
	char **filenames; 
	int verbose;
} option_type;

/* prototypes for functions used across modules */
option_type *get_options(int argc, char **argv);
void usage(char *);
void license(char *);

#ifdef TRY_TO_USE_PTHREAD_DELAY_NP  /* I was using this only on Digital UNIX, but usleep(1) seems to work as well... */
#ifdef HAVE_PTHREAD_DELAY_NP
#define GIVE_IT_UP {struct timespec interval = { 0, Q_NS_DELAY };  pthread_delay_np(&interval); }
#else
#define GIVE_IT_UP usleep(1)
#endif
#else
#define GIVE_IT_UP usleep(1)
#endif

#endif  /* defined MGZIP_H */
