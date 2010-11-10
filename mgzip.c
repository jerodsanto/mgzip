#include <stdio.h> 
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
/* #include <libgen.h>  why is this here? */
#include "config.h"
#include "zlib.h"   /* uses zlib - currently available at http://www.cdrom.com/pub/infozip/zlib/ */
#include "queue.h"  /* Queue model of inter-thread communication */
#include "mgzip.h"
#include "version.h"
#include "die.h"

/* MODIFICATION HISTORY:  --------------------------------- */
/* 09/02/1998 James Lemley -- changed parsearg() calls      */
/*     to getopt() calls -- closer mimics the semantics     */
/*     the real gzip program.                               */
/* -------------------------------------------------------- */
/* 09/14/1998 James Lemley -- cleaned up a little in        */
/*     preparation for posting on the Internet              */
/* -------------------------------------------------------- */
/* 02/23/1999 James Lemley -- added system command to       */
/*     touch output file time to the input file time        */
/* -------------------------------------------------------- */
/* 07/15/1999 James Lemley -- changes to not sling so much  */
/* memory around.                                           */
/* -------------------------------------------------------- */
/* 02/23/2001 James Lemley -- very minor changes for        */
/*     Mac OSX (BSD derived)                                */
/* -------------------------------------------------------- */
/* 03/12/2003 James Lemley -- finally fixed the zero-length */
/*     input file bug.   Now creates a valid empty output.  */
/* -------------------------------------------------------- */
/* 03/12/2003 James Lemley -- fixed a condition where the   */
/*     program would hang with many threads and little data.*/
/* -------------------------------------------------------- */

/* gzip flag byte needs bit 0x04 set for extra field present */
char mgz_header[] = { 0x1f, 0x8b, 0x08, 0x04,  /* stolen from an existing .gz file */
                       0x00, 0x00, 0x00, 0x00,
                       0x00, 0x03, 0x04, 0x00 };  /* modified for a 4 byte extra field */
                      

option_type *options;

#define BUFFER_NULL 0
#define BUFFER_EMPTY 1
#define BUFFER_FULL 2
#define BUFFER_EOF 3
volatile long input_buffer_status[MAX_THREADS*2];  /* needs not to be long, but should be one machine word */
volatile long input_buffer_sizes[MAX_THREADS*2];   /* double buffering requires MAX_THREADS*2 holes */
pthread_mutex_t buffer_status_lock;
char *input_buffers[MAX_THREADS*2];

queuetype *output_queues[MAX_THREADS*2];
queuetype *reader_to_writer_queue;

/* as it turns out, basename() isn't universal.  Hm. */
char *mybasename(char *s) { 
	char *p; 
	p = s + strlen(s)-1;
	while (p >= s && *p != '/')
		p--;
	return p+1;
}	

/* there may be a more standard way to flip the endianness */
/* of a 4 byte word, but this isn't on the critical path   */
/* for performance and it seems to work well.              */
static void longswap(void *ul)
{
    unsigned char *p;
    unsigned char temp;

    p = (unsigned char *) ul;

    /* swap outer two bytes */
    temp = p[0];
    p[0] = p[3];
    p[3] = temp;

    /* swap inner two bytes */
    temp = p[1];
    p[1] = p[2];
    p[2] = temp;
}

/* read from buffer exactly gzip_chunk_size bytes, and write to outq a valid gzip file */
/* prefixed with a 4 byte integer length.   Repeat as long as there is data to compress.  */
int gzip_worker_thread(int worker_number)
{
	/* the input and output queues are set up and ready to go. */

	char *gzip_in_buffer, *buffer2; 
	char *gzip_out_buffer;
	queuetype *outq; 
	
	int length, zlength, outbuffersize;  /* must be 4 bytes */
	unsigned int crc;  /* must be 4 bytes */
	
	z_stream s;
	int err; 
	int outlength;
	int status; 
	int virtualno;
	
#ifdef DEBUG
fprintf(stderr, "Worker %d started. \n", worker_number); 
#endif
	
	outq = output_queues[worker_number];
	
	gzip_in_buffer = (char *) malloc(options->chunk_size);
	buffer2 = (char *) malloc(options->chunk_size);
	
	outbuffersize = (int) ((float)options->chunk_size * 1.10);
	gzip_out_buffer = (char *) malloc(outbuffersize); 
	if (gzip_in_buffer == NULL || gzip_out_buffer == NULL || buffer2 == NULL) 	
		die("malloc() failed in gzip_worker_thread()"); 
	
	/* let the other threads know where our input buffer is */
	pthread_mutex_lock(&buffer_status_lock);

	/* FIX 12/13/2003 -- we need to make sure that the writer hasn't already */
	/* signalled us to quit before we clobber our buffer status.             */
	if (input_buffer_status[worker_number] == BUFFER_EOF || 
		input_buffer_status[worker_number+options->num_threads] == BUFFER_EOF) 
	{ 
#ifdef DEBUG
fprintf(stderr, "Worker %d:  signalled to quit before even starting.  Ending. \n", worker_number); 
#endif
		pthread_mutex_unlock(&buffer_status_lock);
		free(gzip_in_buffer); 
		free(buffer2); 
		free(gzip_out_buffer); 
		pthread_exit(0); 
		return 0;
	}

#ifdef DEBUG
fprintf(stderr, "Worker %d setting my buffer to BUFFER_EMPTY\n", worker_number); 
#endif
	
	input_buffers[worker_number] = gzip_in_buffer;
	input_buffer_status[worker_number] = BUFFER_EMPTY; 
	input_buffers[worker_number+options->num_threads] = buffer2;
	input_buffer_status[worker_number+options->num_threads] = BUFFER_EMPTY; 
	pthread_mutex_unlock(&buffer_status_lock);
	GIVE_IT_UP;  /* end our CPU use for a while (either usleep() or pthread_delay_np()) */

#ifdef DEBUG
fprintf(stderr, "Worker %d registered and running idle loop\n", worker_number); 
#endif
	
	for(;;) {
		do { 
			pthread_mutex_lock(&buffer_status_lock);
			if (input_buffer_status[worker_number] == BUFFER_EOF && 
				input_buffer_status[worker_number+options->num_threads] == BUFFER_EOF)
			{ 
				status = BUFFER_EOF; 
				pthread_mutex_unlock(&buffer_status_lock);
				break;
			}
			if (input_buffer_status[worker_number] == BUFFER_FULL) 
			{ 
				status = BUFFER_FULL;
				virtualno = worker_number;
				length = input_buffer_sizes[virtualno];
				outq = output_queues[virtualno];
				gzip_in_buffer = input_buffers[virtualno];
				pthread_mutex_unlock(&buffer_status_lock);
			}
			else
			if (input_buffer_status[worker_number+options->num_threads] == BUFFER_FULL) 
			{ 
				status = BUFFER_FULL;
				virtualno = worker_number+options->num_threads;
				length = input_buffer_sizes[virtualno];
				outq = output_queues[virtualno];
				gzip_in_buffer = input_buffers[virtualno];
				pthread_mutex_unlock(&buffer_status_lock);
			}
			else	
			{
				status = BUFFER_EMPTY;
				pthread_mutex_unlock(&buffer_status_lock);
				GIVE_IT_UP;   /* wait and loop 'til data or no more */ 
			}
		} while (status == BUFFER_EMPTY); 
		if (status != BUFFER_FULL)
			break;   /* no more... EOF  */ 

#ifdef DEBUG
fprintf(stderr, "worker %d: working on %d bytes...\n", worker_number, length); 
#endif
		s.next_in = (unsigned char *) gzip_in_buffer; 
		s.avail_in = length; 
		s.next_out = (unsigned char *) gzip_out_buffer; 
		s.avail_out = outbuffersize; 
		s.zalloc = NULL;
		s.zfree = NULL;
		s.opaque = NULL;
		s.total_out = 0; 
		
		err = deflateInit2(&s, options->compress_level, Z_DEFLATED, -MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY);
		if (err != Z_OK) 
			die("Error code %d returned from deflateInit2\n", err); 

		err = deflate(&s, Z_FINISH); 
		if (err != Z_STREAM_END) { 
			deflateEnd(&s);
fprintf(stderr, "Funky code is happening. \n"); 
		}
		
		/* get crc32 on input buffer -- this could be in yet another thread. */
		crc = crc32(0, NULL, 0); 
		crc = crc32(crc, gzip_in_buffer, length); 

		/* write length of entire wadge to output queue */
		outlength = s.total_out + sizeof(mgz_header) + sizeof(outlength) + sizeof(crc) + sizeof(length);

#ifdef DEBUG
fprintf(stderr, "worker %d: sending on %d bytes.\n", worker_number, outlength); 
#endif

		/* write length on queue; this doesn't have to be endian neutral because only */
		/* gzip_writer_thread will be reading it natively. 			  */
		enqueue(outq, &outlength, sizeof(outlength), Q_ALL_OR_NOTHING | Q_BLOCK); 
		/* write valid gzip format file to output queue */
		enqueue(outq, mgz_header, sizeof(mgz_header), Q_ALL_OR_NOTHING | Q_BLOCK);
		outlength |= 0x7d000000;  /* mgzip magic byte */ 
#ifdef WORDS_BIGENDIAN
		longswap(&outlength);
#endif
		/* this identifies the .gz file as an mgzip file.  I now have enough   */
		/* information in the file to write a multi-processor capable mgunzip. */
		/* this is the 4 byte extra field we specified in the header.          */
		enqueue(outq, &outlength, sizeof(outlength), Q_ALL_OR_NOTHING | Q_BLOCK); 
		enqueue(outq, gzip_out_buffer, s.total_out, Q_ALL_OR_NOTHING | Q_BLOCK); 
		
#ifdef WORDS_BIGENDIAN 
		longswap(&crc); 
		longswap(&length);	
#endif
		enqueue(outq, &crc, sizeof(crc), Q_ALL_OR_NOTHING | Q_BLOCK); 
		enqueue(outq, &length, sizeof(length), Q_ALL_OR_NOTHING | Q_BLOCK); 
        if (Z_OK != deflateEnd(&s))  
                    die("Error in deflateEnd");

		/* mark our buffer as empty now.     */
		pthread_mutex_lock(&buffer_status_lock);
		if (input_buffer_status[virtualno] == BUFFER_FULL)   /* it is as we left it */
			input_buffer_status[virtualno] = BUFFER_EMPTY;
		pthread_mutex_unlock(&buffer_status_lock);
	}
	/* we're done! */
	/* send EOF on queue */

#ifdef DEBUG
fprintf(stderr, "worker %d: sending EOF\n", worker_number); 
#endif 

	enqueue(outq, NULL, 0, 0);
	
	free(input_buffers[worker_number]);
	free(input_buffers[worker_number+options->num_threads]);
	free(gzip_out_buffer); 
	
#ifdef DEBUG
fprintf(stderr, "Worker %d ending. \n", worker_number); 
#endif
	
	/* we'll let the main thread take care of destroying the queues when she's ready. */
	
	pthread_exit(0); 
	return 0;
}			

int gzip_writer_thread(FILE *outfile)
{
	/* read from ouput_queues as data becomes available, and write */
	/* to the output file */
	int length; 
	char *buffer = NULL; 
	int buffersize = 0; 
	int done, i, l;  
	
#ifdef DEBUG
fprintf(stderr, "gzip_writer_thread started. \n");
fprintf(stderr, "outfile == stdout ? %d\n", outfile == stdout); 
#endif
	
	done = 0;
	do { 
		/* determine which thread was the first one to receive data; we must write */
		/* these in the proper order.                                              */
		l = serve(reader_to_writer_queue, &i, sizeof(i),  Q_ALL_OR_NOTHING | Q_BLOCK);
		if (l <= 0)
		{
			if (queue_eof(reader_to_writer_queue))
				break;
			else 
				die ("Error in gzip_writer_thread: serve error - nothing found and not EOF");
		}
			 
		/* wait for data from queue i to become available */
		l = serve(output_queues[i], &length, sizeof(length), Q_ALL_OR_NOTHING | Q_BLOCK); 
		if (l != sizeof(length))
		{ 
			if (!queue_eof(output_queues[i])) 
				die ("Error in gzip_writer_thread: serve error - nothing found and not EOF"); 
			break;
		}

#ifdef DEBUG
fprintf(stderr, "gzip_writer_thread: next block is %d bytes from queue %d\n", length, i); 
#endif
				
		if (buffersize < length)  /* Oops, must re-allocate the buffer */	
		buffer = realloc(buffer, length);   /* what if we don't have realloc? */
		if (buffer == NULL) 
			die("realloc failed in gzip_writer_thread() for %d bytes\n", length);
		else
			buffersize = length; 
	   	l = serve(output_queues[i], buffer, length, Q_ALL_OR_NOTHING | Q_BLOCK);
	   	if (l != length) 
	   		die ("Error in gzip_writer_thread: serve error - asked for %d bytes; got %d.", length, l);
	   	if (!fwrite(buffer, length, 1, outfile))
	   		die ("Error in gzip_writer_thread: can't write to output file\n"); 

#ifdef DEBUG
fprintf(stderr, "gzip_writer_thread: wrote %d bytes\n", length); 
#endif

	} while (!done); 
	if (buffer != NULL) 
		free (buffer); 

#ifdef DEBUG
fprintf(stderr, "gzip_writer_thread ended. \n");
#endif

	/* main thread will fclose() the output file if she feels like it. */
	pthread_exit(0); 
	return 0;
}

int gzip_reader_thread(FILE *infile)
{
	/* read from input file and write data into input_queues as it will fit. */
	char *buffer; 
	int i, j, length, l; 
	int done = 0;
	struct timespec interval = { 0, Q_NS_DELAY };
	int empty_file = 1; 
	
#ifdef DEBUG
fprintf(stderr, "gzip_reader_thread started. \n");
fprintf(stderr, "infile == stdin ? %d\n", infile == stdin); 
#endif

	do {
		/* find a free buffer to write to */
		do {            
			/* lock buffer status flags */
			pthread_mutex_lock(&buffer_status_lock);
			for (i = 0; i < options->num_threads * 2; i++)		
				if (input_buffer_status[i] == BUFFER_EMPTY)
					break;
			pthread_mutex_unlock(&buffer_status_lock);
			if (i == options->num_threads * 2)
				GIVE_IT_UP;
		} while (i == options->num_threads * 2); 
		
		/* i indexes my empty buffer */	
		if (!(length = fread(input_buffers[i], 1, options->chunk_size, infile)))
		{ 
			/* zero length read.  Check for end of file... */
			if (feof(infile))
				done = 1; 
			else
				die ("Zero length read and not end-of-file -- quitting\n");  /* FIXME */

			/* if this is the first time through (empty file), send a zero length buffer. */
			if (empty_file) 
			{
				pthread_mutex_lock(&buffer_status_lock);
				input_buffer_status[i] = BUFFER_FULL;
				input_buffer_sizes[i] = 0;
				pthread_mutex_unlock(&buffer_status_lock);
				enqueue(reader_to_writer_queue, &i, sizeof(int), Q_ALL_OR_NOTHING | Q_BLOCK);
			}
		}
		else  /* data to process */
		{ 					
			empty_file = 0; 
			pthread_mutex_lock(&buffer_status_lock);
			input_buffer_status[i] = BUFFER_FULL;
			input_buffer_sizes[i] = length;
			pthread_mutex_unlock(&buffer_status_lock);
			
#ifdef DEBUG
fprintf(stderr, "gzip_reader_thread: read %d bytes\n", length); 
#endif
			/* send i as the queue this went to */
			enqueue(reader_to_writer_queue, &i, sizeof(int), Q_ALL_OR_NOTHING | Q_BLOCK);
#ifdef DEBUG
fprintf(stderr, "gzip_reader_thread: sent to worker %d\n", i); 
#endif			
		}		 
	} while (!done); 

	enqueue(reader_to_writer_queue, NULL, 0, 0);  /* send EOF on queue to writer thread */
	
	/* set all buffers to EOF.  This will cause all waiting workers to exit. */
	do {	
		done = 1; 
		pthread_mutex_lock(&buffer_status_lock);
		for (i = 0; i < options->num_threads * 2; i++)
			if (input_buffer_status[i] == BUFFER_EMPTY || input_buffer_status[i] == BUFFER_NULL)
				input_buffer_status[i] = BUFFER_EOF;  /* mark all input buffers as getting no more */
			else
				if (input_buffer_status[i] == BUFFER_FULL)
					done = 0;  /* must come back for this one */
		pthread_mutex_unlock(&buffer_status_lock);
		if (!done) 
			GIVE_IT_UP;
	} while (!done);

#ifdef DEBUG
fprintf(stderr, "gzip_reader_thread ended. \n");
#endif
	pthread_exit(0);
	return 0;  
}

void usage(char *name) 
{
	/* this could stand to be rewritten. */
	
	fprintf(stderr, "mgzip, a multi-processor capable .gz file creator. \n\n"); 
	fprintf(stderr, "%s version %s build %d on %s\n", mybasename(name), VERSION, BUILD, __DATE__); 
	fprintf(stderr, "Usage: %s [-cfp19L] [-t N] [-C N] [file...]  \nwhere -1 is fastest and -9 is best compression (default is -%d)\n", 
					name, DEFAULT_COMPRESSION_LEVEL);
	fprintf(stderr, "and -t N is the number of worker threads to use (default is -t %d)\n", DEFAULT_THREADS);
	fprintf(stderr, "If filename is omitted, this program compresses from the standard input to the\nstandard output. \n"); 
    fprintf(stderr, "More parameters:\n  -c writes compressed data to standard out and keeps the original\n"); 
	fprintf(stderr, "  -C N where N is a size in bytes will change the default\n    chunk size (now %d)\n", CHUNK_SIZE);
	fprintf(stderr, "  -f forces compression to a terminal \n  -p preserves the input file after compression\n");
	fprintf(stderr, "  -L prints the license and quits. \n");  
	fprintf(stderr, "\nNote: this program only compresses files; you must use gzip -d (or gunzip) to \n"); 
	fprintf(stderr, "uncompress the resulting compressed files.  This program is neither derived from\n"); 
	fprintf(stderr, "nor directly related to \"gzip\" by Jean-loup Gailly and Mark Adler, although it\n"); 
	fprintf(stderr, "does use the \"zlib\" compression library by the same authors under the GPL. \n\n"); 
	exit(1); 
}

void license(char *name) 
{
	
	fprintf(stderr, "mgzip, a multi-processor capable .gz file creator. \n\n"); 
	fprintf(stderr, "%s version %s build %d on %s\n", mybasename(name), VERSION, BUILD, __DATE__); 
    fprintf(stderr, "Copyright (C) 1998 James Lemley <james@lemley.net>\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This program is free software; you can redistribute it and/or modify\n");
    fprintf(stderr, "it under the terms of the GNU General Public License as published by\n");
    fprintf(stderr, "the Free Software Foundation; either version 2 of the License, or\n");
    fprintf(stderr, "(at your option) any later version.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "This program is distributed in the hope that it will be useful,\n");
    fprintf(stderr, "but WITHOUT ANY WARRANTY; without even the implied warranty of\n");
    fprintf(stderr, "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n");
    fprintf(stderr, "GNU General Public License for more details.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "You should have received a copy of the GNU General Public License\n");
    fprintf(stderr, "along with this program; if not, write to the Free Software\n");
    fprintf(stderr, "Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.\n");

	exit(0); 
}
	
/* infile and outfile are already open.  spawn threads, do work, clean up. */
/* This function may be called several times through the life of the process. */
void compress_infile_to_outfile (FILE *infile, FILE *outfile)
{ 
	pthread_t worker_threads[MAX_THREADS]; 
	pthread_t reader_thread; 
	pthread_t writer_thread; 
	int i, ret; 
	void *thread_ret;
#ifdef _AIX
	pthread_attr_t attr;
	pthread_attr_init(&attr); 
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_UNDETACHED); 
#endif

	
   	if (pthread_mutex_init(&buffer_status_lock, NULL)) /* any return value is failure */
		die("pthread_mutix_init failed"); 

 	for (i = 0; i < options->num_threads * 2; i++)
 	{
 		/* make queues that workers will use to read from and write to */
 		output_queues[i] = newqueue();
 		if (output_queues[i] == NULL)
 			die ("Queue create error");
		input_buffer_status[i] = BUFFER_NULL;  /* no threads yet - don't have to lock */
		input_buffer_sizes[i] = 0;
 	}
 	reader_to_writer_queue = newqueue();
 	if (reader_to_writer_queue== NULL)
 		die ("Queue create error");

 	/* start the reader thread */
#ifdef _AIX
 	ret = pthread_create(&reader_thread, &attr, (void *)(void *)gzip_reader_thread, (void *)infile);
#else
	ret = pthread_create(&reader_thread, NULL, (void *)(void *)gzip_reader_thread, (void *)infile);
#endif 
 	if (ret) die("pthread_create returns %d\n", ret);

 	/* start worker threads */
 	for (i = 0; i < options->num_threads; i++)
 	{
#ifdef _AIX
 		ret = pthread_create(&worker_threads[i], &attr, (void *)(void *)gzip_worker_thread, (void *) i);
#else
		ret = pthread_create(&worker_threads[i], NULL, (void *)(void *)gzip_worker_thread, (void *) i);
#endif
 		if (ret) die("pthread_create returns %d\n", ret);
 	}

 	/* start writer thread */
#ifdef _AIX
 	ret = pthread_create(&writer_thread, &attr, (void *)(void *)gzip_writer_thread, (void *)outfile);
#else
	ret = pthread_create(&writer_thread, NULL, (void *)(void *)gzip_writer_thread, (void *)outfile);
#endif
 	if (ret) die("pthread_create returns %d\n", ret);

 	/* wait on reader thread */
	pthread_join(reader_thread, &thread_ret);

 	/* wait on workers */
 	for (i = 0; i < options->num_threads; i++)
 	{
 		pthread_join(worker_threads[i], &thread_ret);
 	}
 	/* wait on writer */
 	pthread_join(writer_thread, &thread_ret);

 	/* free memory associated with communication lines */
 	freequeue(reader_to_writer_queue);
 	for (i = 0; i < options->num_threads; i++)
 	{
 		freequeue(output_queues[i]);
 	}
	pthread_mutex_destroy(&buffer_status_lock);
}

void check_for_endianness()
{
	int i = 1;
	char *p;

	p = (char *) &i;
#ifndef WORDS_BIGENDIAN
	if (!*p)
		die("Compiled without WORDS_BIGENDIAN, but apparently so.\n");
#else
	if (*p)
		die("Apparently little endian, but WORDS_BIGENDIAN is defined.\n");
#endif
}	

int main(int argc, char **argv) 
{
	int i; 
	int something_happened = 0;
	int return_code = 0; 
    
	FILE *infile = NULL;
	FILE *outfile = NULL;

	options = get_options(argc, argv);

	check_for_endianness();
	
	if (options->infile == stdin) 
	{
	 	if (isatty(fileno(options->outfile)) && !options->force_stdout)
	 	{
	 		die("compressed data not written to a terminal. Use -f to force compression.\n"
	 			"For help, type: %s -h\n", mybasename(argv[0]));
	 	}

		compress_infile_to_outfile(options->infile, options->outfile); 
		something_happened = 1; 
	}
	else
	{
		for (i = 0; i < options->num_files; i++) 
		{		
			int my_preserve_infile = options->preserve_infile; 
	 		struct stat st;
	 		char outfilename[255];  /* THIS IS BAD. */

			outfile = options->outfile;
	 		/* try to stat() this input file */
	 		if (stat(options->filenames[i], &st))
	 		{
	 			fprintf(stderr, "%s: file not found\n", options->filenames[i]); 
				if (return_code < 1) 	
					return_code = 1;  /* minor error */
				continue; 
	 		}
	 		if ((st.st_mode & S_IFMT) & (S_IFREG | S_IFIFO))  /* regular file or fifo queue */
	 		{
	 			if (NULL == (infile = fopen(options->filenames[i], "rb")))
	 			{
	 				if (return_code < 1) 
	 					return_code = 1;
	 				fprintf(stderr, "Can't open input file %s\n", options->filenames[i]);
					continue;
				}
	 				
	 			if (outfile != stdout)
	 			{
	 				sprintf(outfilename, "%s.gz", options->filenames[i]);
	 				if (NULL == (outfile = fopen(outfilename, "wb")))
	 					die("Can't open output file %s\n", outfilename);
	 			}
	 				
	 			if ((st.st_mode & S_IFMT) != S_IFREG)
	 				my_preserve_infile = 1;  /* only erase regular files upon completion */
	 		}
	 		else
	 		{
	 			if (return_code < 1) 	
	 				return_code = 1; 
	 			fprintf(stderr, "%s is not a regular file or fifo queue. \n", options->filenames[i]); 
				continue;
	 		}

		 	if (isatty(fileno(outfile)) && !options->force_stdout)
		 	{
	 			die("compressed data not written to a terminal. Use -f to force compression.\n"
	 				"For help, type: %s -h\n", mybasename(argv[0]));
	 		}

			compress_infile_to_outfile(infile, outfile); 
			something_happened = 1; 
		
	 		if (infile != stdin)
	 			fclose(infile);
	 		
		 	if (outfile != stdout)
		 	{
	 			fclose(outfile);
				if (infile != stdin) 
				{
					char command[256];  /* bad bad */
					sprintf(command, "touch -r %s %s", options->filenames[i], outfilename);
					system(command); 
				}  
	 			if (!my_preserve_infile)
	 				unlink(options->filenames[i]);
		 	}
		}
	}
	if (!something_happened)  /* file not found, etc. */
		usage(argv[0]);
	
	return 0;  /* done */	
}

