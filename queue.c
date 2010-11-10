/* define a thread-safe simple byte queue for quickly transmitting loads of data between threads */
/* note: this isn't the most efficient inter-thread communication scheme I can think of, but     */
/* I need something that will act like a buffering queue and transfer lots of data fast.         */
/* If you know of a better and fairly portable way to achieve the same, please let me know or    */
/* better yet write it and send me the patches :)                                                */

/* 07-15-1999                                                                               */
/* fixed buffer wrap-around bug when feeding chunks of exactly BUFFEREXTENT size to a queue */

#include <stdio.h> 
#include <errno.h> /* for testing */
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "config.h"
#include "queue.h"
#include "mgzip.h" /* for the GIVE_IT_UP macro */


queuetype *newqueue()
{
	queuetype *ret; 
	
	ret = (queuetype *) malloc(sizeof(queuetype)); 
	if (ret == NULL) 
		return NULL;
	
	ret->buffer = malloc(BUFFEREXTENT); 
	if (ret->buffer == NULL) 
	{
		free (ret); 
		return NULL; 
	}
	ret->buffersize = BUFFEREXTENT; 
	ret->inp = ret->outp = ret->buffer; 
	ret->bytecount = 0; 
	ret->queue_eof = 0;
#ifdef HAVE_PTHREADS	
	if (pthread_mutex_init(&ret->lock, NULL)) /* any return value is failure */
	{
		free (ret->buffer); 
		free (ret);
		return NULL; 
	}
#endif	
	return ret; 
}

void freequeue(queuetype *q)
{
	free(q->buffer); 
#ifdef HAVE_PTHREADS
	pthread_mutex_destroy(&q->lock);
#endif
	free(q);
}

int enqueue(queuetype *q, void *bytes, int length, int flags)
{
	/* returns number of bytes enqueued */
	
	/* check to see if this is a reasonable request */
	if (length > BUFFEREXTENT * MAXEXTENTS)
		return -1;  /* this will never work */
	
	/* lock the queue */
	pthread_mutex_lock(&q->lock);

	/* if bytes is NULL, set the EOF flag */
	if (bytes == NULL) 
	{
		q->queue_eof = 1; 
		pthread_mutex_unlock(&q->lock);
		return 0; 
	}
	else 	
		q->queue_eof = 0;  /* reset EOF flag if there's more data */
	
	/* can the queue hold this number of bytes? */
	if (q->buffersize - q->bytecount < length)  /* increase buffer size if possible */
	{
		if (q->buffersize < BUFFEREXTENT * MAXEXTENTS)  /* yay - expand buffer */
		{
			int new_extents; 
			char *temp_buffer; 
			
			new_extents = (q->buffersize / BUFFEREXTENT) + 1 + length / BUFFEREXTENT;   /* how many we want */
			if (new_extents > MAXEXTENTS) 
				new_extents = MAXEXTENTS;  /* darn. */
			/* re-allocate buffer */
			temp_buffer = (char *)malloc( new_extents * BUFFEREXTENT);
			if (temp_buffer) /* if success in expanding buffer */
			{
				if (q->outp >= q->inp && q->bytecount)  /* wrapped around */
				{
					memcpy(temp_buffer, q->outp, q->buffer + q->buffersize - q->outp); 
					memcpy(temp_buffer + (q->buffer + q->buffersize - q->outp), 
							q->buffer, q->inp - q->buffer); 
				}
				else
				{
#ifdef DEBUG				
					fprintf(stderr, "New buffer size: %d  Old buffer size: %d ",  new_extents * BUFFEREXTENT, 
							q->buffersize); 
					fprintf(stderr, "Copying %d bytes from offset %d of old buffer to new buffer\n", 
							q->bytecount, q->outp - q->buffer); 
					fprintf(stderr, "Old buffer at %x, inp at %x, outp at %x\n", q->buffer, q->inp, q->outp);
#endif 
					memcpy(temp_buffer, q->outp, q->bytecount); 
				}
					
				q->outp = temp_buffer; 
				q->inp = q->outp + q->bytecount; 
							
				free(q->buffer); 
				q->buffer = temp_buffer; 
				q->buffersize = new_extents * BUFFEREXTENT;
			}
		}
	}
	/* buffer is either large enough or as large as we can make it. */ 		
	
	while ((flags & Q_BLOCK) && length > (q->buffersize - q->bytecount))
	{
/* fprintf(stderr, "write wait -- no space left in queue (%d buffer)\n", q->buffersize); 
*/
		/* unlock the queue, wait a while, and relock the queue */
		pthread_mutex_unlock(&q->lock);
 		GIVE_IT_UP;  /* release the rest of our timeslice */
		pthread_mutex_lock(&q->lock);
		/* until there's enough space in the queue to complete the request. */
	}
	
	if (length > (q->buffersize - q->bytecount))
	{
		if (flags & Q_DO_WHAT_YOU_CAN)
			length = (q->buffersize - q->bytecount);
		else
			length = 0; 
	}
	if (!length) 
	{
		pthread_mutex_unlock(&q->lock);
		return 0; 
	}
	
	/* OK, we can now add length bytes to the queue.  There's room, trust me.  */
	if (q->inp + length >= q->buffer + q->buffersize)  /* have to wrap */
	{
		int length1 = q->buffer + q->buffersize - q->inp;
		int length2 = length - length1; 
		
		memcpy(q->inp, bytes, length1); 
		memcpy(q->buffer, ((char *)bytes) + length1, length2);
		q->inp = q->buffer + length2; 
	}
	else
	{
		/* straight copy  */
		memcpy(q->inp, bytes, length); 
		q->inp += length; 
	}
	q->bytecount += length; 

#ifdef BUFFER_CAN_SHRINK	
	if (q->buffersize != BUFFEREXTENT)
		if (q->bytecount * 10 < q->buffersize)  /* if under 10 percent used */
		{
			if (++q->queue_oversized == 50)    /* increment oversized flag */
			{
				char *temp_buffer;		/* shrink queue buffer by 50% */
				int new_extents = (q->buffersize / BUFFEREXTENT) / 2;
				if (NULL != (temp_buffer = (char *) malloc(new_extents * BUFFEREXTENT)))
				{
/* fprintf(stderr, "Shrinking buffer from %d bytes to %d bytes. \n", q->buffersize, new_extents * BUFFEREXTENT);
*/
					if (q->outp > q->inp)  /* two copies */
					{
						int length1 = q->buffer + q->buffersize - q->outp;
						memcpy(temp_buffer, q->outp, length1); 
						memcpy(temp_buffer + length1, q->buffer, q->bytecount - length1); 
					}
					else
					{
						memcpy(temp_buffer, q->outp, q->bytecount);
					}
					q->outp = temp_buffer;
					q->inp = temp_buffer + q->bytecount; 
					q->buffersize = new_extents * BUFFEREXTENT;
					free(q->buffer);
					q->buffer = temp_buffer; 
				}	
			}
		}
		else
			if (q->bytecount * 2 > q->buffersize)  /* if over 50 percent used */
			 	q->queue_oversized = 0;            /* clear oversized flag    */
#endif
	
	pthread_mutex_unlock(&q->lock);

	return length;
}

int serve(queuetype *q, void *buffer, int length, int flags) 
{
	if (!length) 
		return 0; 
		
	/* lock the queue */
	pthread_mutex_lock(&q->lock);
	
	/* if EOF on queue and the queue is empty, return -1 no matter what */
	if (q->queue_eof && !q->bytecount) 
	{
		pthread_mutex_unlock(&q->lock);
		return -1;
	}
	
	/* is there this much or any data on the queue? */
	if (flags & Q_BLOCK) 
		while ( ((flags & Q_ALL_OR_NOTHING) && q->bytecount < length)  || 
			    ((flags & Q_DO_WHAT_YOU_CAN) && q->bytecount == 0) ) 
		{
/* fprintf(stderr, "read wait -- no data left in queue (%d buffer)\n", q->buffersize); 
*/
			/* unlock the queue, wait a while, and relock the queue */
			pthread_mutex_unlock(&q->lock);
			GIVE_IT_UP;
			pthread_mutex_lock(&q->lock);
			/* until there's enough data in the queue to complete the request. */
			/* or eof has been set */
			if (q->queue_eof) 
				break;
		}
	
/* 	if ((q->bytecount < length && (flags & Q_ALL_OR_NOTHING)) || !q->bytecount)  */

	if (!q->bytecount)
	{
		pthread_mutex_unlock(&q->lock);
		return 0;
	}	 
	
	/* copy what we have, up to length bytes, to the output buffer */
	if (length > q->bytecount) 
		length = q->bytecount; 
		
	if (q->outp + length >= q->buffer + q->buffersize)  /* wrapped */
	{
		int length2 = (q->outp + length) - (q->buffer + q->buffersize);
		int length1 = length - length2;
		
		memcpy(buffer, q->outp, length1); 
		memcpy(((char *)buffer)+length1, q->buffer, length2); 
		q->outp = q->buffer + length2; 
	}
	else
	{
		memcpy(buffer, q->outp, length); 
		q->outp += length; 
	}
	q->bytecount -= length; 
	pthread_mutex_unlock(&q->lock);
	return length; 
}

int queue_eof(queuetype *q) 
{
	int ret; 
	pthread_mutex_lock(&q->lock);
	ret = q->queue_eof && (q->bytecount == 0);
	pthread_mutex_unlock(&q->lock);
	return ret; 
}	

/* ************************************************************************** */
/* STANDALONE TESTING BELOW THIS POINT                                        */
/* ************************************************************************** */
#ifdef QUEUETEST

void slam_stuff_in_queue(queuetype *q) 
{
	char buffer[19395];
	int l;
	
	fprintf(stderr, "Whaddyaknow, I'm in slam_stuff_in_queue. Starting the fread...\n"); 
	
	while (l = fread (buffer, 1, sizeof(buffer), stdin)) 
		if (0 >= enqueue(q, buffer, l, Q_ALL_OR_NOTHING | Q_BLOCK)) 
		{
			fprintf(stderr, "Enqueue failed for %d bytes! \n", l);
			break;
		}

	fprintf(stderr, "enqueueing a NULL for EOF...\n"); 
	/* Try to enqueue a NULL -- sends EOF on queue */
	enqueue(q, NULL, 0, 0);

	fprintf(stderr, "exiting!\n"); 
	pthread_exit((void *)0);

	return; 
}
	
void read_stuff_from_queue(queuetype *q) 
{
	char buffer[23913];
	int l;
	
	fprintf(stderr, "Omigosh, I'm in read_stuff_from_queue. Starting the serve()...\n"); 
	
	do { 
		l = serve(q, buffer, sizeof(buffer), Q_DO_WHAT_YOU_CAN | Q_BLOCK); 
		if (l <= 0)  /* error or EOF */
			break;
		else
			fwrite(buffer, l, 1, stdout); 
	} while (l); 

    fprintf(stderr, "Got 0 bytes from queue and queue_eof returns %d. Exiting... \n", queue_eof(q) ); 
    	           
	pthread_exit((void *)0);
	            
	return;
}

int main()
{
	pthread_t threads[2]; 
	int i; 
	int ret; 
	queuetype *q; 
	void *thread_ret;
	
	fprintf(stderr, "creating new queue...\n"); 
	q = newqueue(); 
	fprintf(stderr, "have new queue. \n"); 
	if (q == NULL) 
		die ("bad queue\n"); 
	i = 0; 
	ret = pthread_create(&threads[i], NULL, (void *)(void *)slam_stuff_in_queue, (void *)q);
	fprintf(stderr, "back from pthread_create 1; ret is %d, errno is %d\n", ret, errno); 
	errno = 0;
	i = 1; 
	ret = pthread_create(&threads[i], NULL, (void *)(void *)read_stuff_from_queue, (void *)q);
	fprintf(stderr, "back from pthread_create 2; ret is %d, errno is %d\n", ret, errno); 

	fprintf(stderr, "waiting on thread 1 \n"); 
	pthread_join(threads[0], &thread_ret);
	fprintf(stderr, "waiting on thread 2 \n"); 
	pthread_join(threads[1], &thread_ret); 

	fprintf(stderr, "freeing queue...\n"); 
	freequeue(q); 
	fprintf(stderr, "done. \n"); 

	return 0; 
}
#endif
	
