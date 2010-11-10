#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "die.h"
#include "mgzip.h"
#include "queue.h"  /* for definitions of BUFFEREXTENT and MAXEXTENTS */

extern char *optarg;  /* these are defined by UNIX in the getopt library */
extern int optind;    
extern int opterr;
extern int optopt;
        
/* get_options will populate an option_type structure and return */
/* a pointer to the filled-in structure for the rest of the      */
/* program to use.                                               */
option_type *get_options(int argc, char **argv) 
{
	static option_type *opt = NULL;
	int c, errflag; 

	if (opt) 
		free(opt);

	opt = (option_type *) malloc(sizeof(option_type)); 
	if (opt == NULL) 
		die("Memory allocation failed.\n"); 

	memset(opt, 0, sizeof(option_type)); 
			
	optarg = NULL;  
	errflag = 0; 
	while (!errflag && ((c = getopt(argc, argv, "pcfhLt:C:0123456789v"))))
	{
		switch(c) 
		{
			case 't' : opt->num_threads = atoi(optarg); 
						break;
			case 'C' : opt->chunk_size = atoi(optarg); 
						break;
			case 'p' : opt->preserve_infile = 1; 
						break;
			case 'c' : opt->preserve_infile = 1; 
						opt->outfile = stdout;		
						break;
			case 'f' : opt->force_stdout =1; 
						break;
			case 'h' : usage(argv[0]);
						break;
			case 'L' : license(argv[0]);
						break;
			case '0': 
			case '1': 
			case '2': 
			case '3': 
			case '4': 
			case '5': 
			case '6': 
			case '7':
			case '8': 
			case '9':  opt->compress_level = c - '0';
						break;
			case 'v':  opt->verbose = 1; 
						break;
						
			default: 	errflag++;
		} 
	}
	if (c != -1 && errflag) 
		die("use %s -h for help\n", argv[0]); 

				
	/* make sure things in the opt structure are reasonable; default any that are not*/
	if (opt->num_threads <= 0) 
		opt->num_threads = DEFAULT_THREADS;  /* default to two worker threads */
	if (opt->num_threads > MAX_THREADS) 
		opt->num_threads = MAX_THREADS; 

	if (opt->chunk_size < 1024 || opt->chunk_size >  (BUFFEREXTENT * MAXEXTENTS - 1024)) 
		opt->chunk_size = CHUNK_SIZE;
	
	if (opt->compress_level < 1 || opt->compress_level > 9) 
		opt->compress_level = DEFAULT_COMPRESSION_LEVEL;
	
	if (optind == argc)   /* no more options on command line */
	{
		opt->infile = stdin; 
		opt->outfile = stdout;
	}
	else
	{
		opt->num_files = argc - optind; 
		opt->filenames = argv + optind; 
	}

	if (opt->verbose) 
	{ 
		fprintf(stderr, "Options summary: \n\n"); 
    	fprintf(stderr, "int force_stdout;    %d\n", opt->force_stdout);
    	fprintf(stderr, "int preserve_infile; %d\n", opt->preserve_infile);
    	fprintf(stderr, "int compress_level;  %d\n", opt->compress_level);
    	fprintf(stderr, "int num_threads;     %d\n", opt->num_threads);
    	fprintf(stderr, "int chunk_size;      %d\n", opt->chunk_size);
    	fprintf(stderr, "FILE *infile;        %p\n", opt->infile);
    	fprintf(stderr, "FILE *outfile;       %p\n", opt->outfile);
    	fprintf(stderr, "int num_files;       %d\n", opt->num_files);
		fprintf(stderr, "char **filenames;    %p\n\n", opt->filenames); 
	}	

	return opt;  /* done */	
}

#ifdef TEST_OPTIONS
int main(int argc, char **argv)
{ 
	get_options(argc, argv); 
	return 0;
}
#endif
