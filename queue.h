/* define a thread-safe simple byte queue for quickly transmitting loads of data between threads */
#ifndef QUEUE_H
#define QUEUE_H

#define HAVE_PTHREADS 
/* #define BUFFER_CAN_SHRINK  */
#define BUFFEREXTENT 32768

#ifdef BUFFER_CAN_SHRINK
#define MAXEXTENTS 32 /*  32 * 32768 == 1 megabyte - maximum buffer size for a single queue */
#else
#define MAXEXTENTS 16
#endif

#include <pthread.h>

#define Q_DO_WHAT_YOU_CAN 1  /* write or read as much data as possible to/from the queue */
#define Q_ALL_OR_NOTHING  2  /* read or write all data requested or none at all          */
#define Q_BLOCK           4  /* block until complete ( or EOF in serve() )               */

#define Q_USLEEP_TIMEOUT 10000
#define Q_NS_DELAY 10000000  /* 10 ms, 10000 us, 10000000 ns */

typedef struct { 
	char *buffer; 
	int buffersize;
	char *inp, *outp;
	int bytecount; 
	pthread_mutex_t lock;
	int queue_eof;
	int queue_oversized;
} queuetype;

queuetype *newqueue();
void freequeue(queuetype *q);
int enqueue(queuetype *q, void *bytes, int length, int flags);
int serve(queuetype *q, void *buffer, int length, int flags);
int queue_eof(queuetype *q);

#endif
