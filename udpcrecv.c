/*
 * store received udp packets interleaved by ip address
 */

#define _FILE_OFFSET_BITS	64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <pthread.h>

#include <assert.h>

#include "helper.h"
#include "tsfifo.h"
#include "mempool.h"


#define KiB		1024
#define MiB		(1024 * KiB)
#define GiB		(1024 * MiB)


const unsigned int	RECVBUF_SIZE 	= (16 * KiB);
const unsigned int	MEMBLK_SIZE		=  (2 * MiB);
const unsigned int	FIFO_DEPTH		= 256;


/*
 * global running flag
 */
volatile sig_atomic_t g_finished = 0;


/*
 * receive thread context
 */
typedef struct recvstat_s
{
	int			sockfd;
	mempool_t	* pool;
	tsfifo_t	* writeq;		/* write queue */
	int			chan_idx;		/* index of selected channel */
	int			wait_pps;		/* data valid after sn reset to 0 */
	uint64_t	pkt_lost;
	uint64_t	bytes;			/* total bytes received */
	pthread_t	tid;
} recvstat_t;

/*
 * write thread context
 */
typedef struct writestat_s
{
	mempool_t	* pool;
	tsfifo_t	* writeq;
	uint64_t	bytes;			/* total bytes written */
	pthread_t	tid;
} writestat_t;



/* ATTENTION: use static memory for generated string!
 * So it can NOT be used in single printf function twice. */
static inline char * dynamic_tgmk( double bps )
{
#define	BUFSIZE	64
	static char buf[BUFSIZE];

	if( bps >= 1E12 )
		snprintf(buf, BUFSIZE, "%7.3f T", (bps + .5E9) / 1E12);
	else if( bps >= 1E9 )
		snprintf(buf, BUFSIZE, "%7.3f G", (bps + .5E6) / 1E9 );
	else if( bps >= 1E6 )
		snprintf(buf, BUFSIZE, "%7.3f M", (bps + .5E3) / 1E6);
	else if( bps >= 1E3 )
		snprintf(buf, BUFSIZE, "%7.3f K", bps / 1E3);
	else
		snprintf(buf, BUFSIZE, "%f", bps);

	return buf;
#undef	BUFSIZE
}


static inline void block_sigint( void )
{
	sigset_t sigset;
	sigemptyset( &sigset );
	sigaddset( &sigset, SIGINT );
	pthread_sigmask( SIG_BLOCK, &sigset, NULL );
}


void stop_processing( int signum )
{
    fprintf( stderr, "\nCaught signal %d, stop record.\n", signum );
    g_finished = 1;
}


static inline memblk_t * record_packet( recvstat_t * p_rs, memblk_t * p_blk, unsigned char * p_pkt, ssize_t recv_len )
{
	uint16_t	* p_samp = (uint16_t *)(p_pkt + sizeof(uint64_t) + p_rs->chan_idx * 2);
	uint16_t	* p_dest = (uint16_t *)(p_blk->buf + p_blk->used);
	size_t		i, nsamp_per_pkt = (recv_len - sizeof(uint64_t)) / (32 * 2);

	for( i = 0; i < nsamp_per_pkt; i++ )
	{
		*p_dest = *p_samp;
		p_samp += 32;
		p_dest++;
	}

	p_blk->used += nsamp_per_pkt * 2;
	p_rs->bytes += recv_len;

	assert( p_blk->used <= p_blk->size );

	if( p_blk->used == p_blk->size )
	{
		tsfifo_put( p_rs->writeq, (uintptr_t)p_blk );
		p_blk = (memblk_t *)mempool_alloc( p_rs->pool );
		if( NULL == p_blk )
		{
			fprintf( stderr, "\nmempool overflow!\n" );
			exit( ENOMEM );
		}
		assert( p_blk->used == 0);
	}

	return p_blk;
}


static inline memblk_t * fill_lost_packets( recvstat_t * p_rs, memblk_t * p_blk, size_t nlost, ssize_t recv_len )
{
	size_t	lost_len, remain;
	size_t	nsamp_per_pkt = (recv_len - sizeof(uint64_t)) / (32 * 2);

	p_rs->pkt_lost += nlost;
	p_rs->bytes += nlost * recv_len;

	/* FIXME: add zero or noise packets to write buffer */
	/* Always assume lost packets has the same size as the current one */
	lost_len = nlost * nsamp_per_pkt * 2;
	if( lost_len >= MEMBLK_SIZE )
		fprintf( stderr, "\ntoo many lost: %zu\n", lost_len );

	if( p_blk->used + lost_len < p_blk->size )
	{
		p_blk->used += lost_len;
		return p_blk;
	}

	/* lost packets across memblk boundary */
	remain = p_blk->used + lost_len;
	do {
		p_blk->used = p_blk->size;
		tsfifo_put( p_rs->writeq, (uintptr_t)p_blk );
		p_blk = (memblk_t *)mempool_alloc( p_rs->pool );
		if( NULL == p_blk )
		{
			fprintf( stderr, "\nmempool overflow!\n" );
			exit( ENOMEM );
		}
		assert( p_blk->used == 0);
		remain -= p_blk->size;
	} while( remain >= p_blk->size );

	p_blk->used = remain;

	return p_blk;
}


void * receive_thread( void * args )
{
	recvstat_t	* p_rs;
	memblk_t	* p_blk;
	uint64_t	prev_cnt = 0, curr_cnt = 0;
	ssize_t		recv_len;
	//size_t		payload_len, nsamp_per_pkt;

	unsigned char p_pkt[RECVBUF_SIZE] __attribute__ ((aligned (64)));


	block_sigint( );

	p_rs  = (recvstat_t *)args;

	/* XXX */
	//mempool_t * pool = p_rs->pool;
	//memset( pool->membuf, 0, pool->pool_size * pool->block_size );

	p_blk = (memblk_t *)mempool_alloc( p_rs->pool );

	assert( p_blk != NULL );
	assert( p_blk->used == 0 );

	/* waiting for start capture */
	if( p_rs->wait_pps )
	{
		while( curr_cnt >= prev_cnt )
		{
			recv_len = recv( p_rs->sockfd, p_pkt, RECVBUF_SIZE, 0 );
			if( recv_len <= 0 )
				continue;
			prev_cnt = curr_cnt;
			curr_cnt = *(uint64_t *)p_pkt;
		}
	}
	/* get the 1st packet and extract serial number */
	else
	{
		recv_len = 0;
		while( recv_len <= 0 )
			recv_len = recv( p_rs->sockfd, p_pkt, RECVBUF_SIZE, 0 );
		curr_cnt = *(uint64_t *)p_pkt;
	}

	p_blk = record_packet( p_rs, p_blk, p_pkt, recv_len );
	prev_cnt = curr_cnt;

	while( !g_finished )
	{
		// recv_len = recv( p_rs->sockfd, p_pkt, RECVBUF_SIZE, MSG_DONTWAIT );
		recv_len = recv( p_rs->sockfd, p_pkt, RECVBUF_SIZE, 0 );
		if( recv_len <= 0 )
			continue;

		/* extract packet counter from first 8 bytes */
		curr_cnt = *(uint64_t *)p_pkt;

		//payload_len = recv_len - sizeof(uint64_t);
		//nsamp_per_pkt = payload_len / (32 * 2);

		/* check packet counter to see if there are some packet missed */
		if( prev_cnt + 1 != curr_cnt )
		{
			size_t	nlost = (size_t)(curr_cnt - prev_cnt - 1);
			p_blk = fill_lost_packets( p_rs, p_blk, nlost, recv_len );
			//
			/*
			fprintf( stderr, "[%d] recv_len=%zd prev_cnt=%"PRIu64" curr_cnt=%"PRIu64" packet lost %"PRIu64" delay %"PRIu64"\n",
					 (unsigned int)p_rs->tid, recv_len, prev_cnt, curr_cnt, nlost, mdate() - start);
			fflush( stderr );
			//*/

		} /* packet lost */

		p_blk = record_packet( p_rs, p_blk, p_pkt, recv_len );
		prev_cnt = curr_cnt;

	}	/* while( 1 ) */


	tsfifo_put( p_rs->writeq, (uintptr_t)p_blk );

	return NULL;
}


/*
 * Write thread: write received memory block to file
 */
void * write_thread( void * args )
{
	writestat_t	* p_ws;

	block_sigint( );

	p_ws = (writestat_t *)args;

	while( !g_finished )
	{
		memblk_t * p_blk = (memblk_t *)tsfifo_get( p_ws->writeq );
		if( NULL == p_blk )
		{
			usleep( 1 );
			continue;
		}

		assert( p_blk->used > 0 );

		fwrite( p_blk->buf, p_blk->used, 1, stdout );

		p_ws->bytes += p_blk->used;
		mempool_free( p_ws->pool, p_blk );
	}

	return NULL;
}



int main( int argc, char *argv[] )
{
	int				ret;
	int				sockfd;
	char			local_ip[16], local_port[16];

	tsfifo_t		* writeq;
	mempool_t		* pool;
	recvstat_t		recvstat;
	writestat_t		writestat;
	void			* p_exitcode;

	int				chan_idx;
	int				wait_pps = 0;


	/*
	 * Process arguments
	 */

	if( argc != 3 || !parse_ipv4_addr( argv[1], local_ip, 16, local_port, 16 ) )
	{
		fprintf( stderr, "\nUsage: %s <local_ip_0:local_port_0> <channel_index>\n\n", argv[0] );
		return EXIT_FAILURE;
	}

	chan_idx = atoi( argv[2] );
	fprintf( stderr, "Receiving samples from %s(%s) channel index %d.\n", local_ip, local_port, chan_idx );


	/*
	 * Create Sockets
	 */

	sockfd = bind_to_local( local_ip, local_port );
	if( sockfd == -1 )
	{
		fprintf( stderr, "failed to bind to %s:%s\n", local_ip, local_port );
		return EXIT_FAILURE;
	}


	/*
	 * create memory pool and write quque
	 */

	pool = mempool_create( FIFO_DEPTH, MEMBLK_SIZE );
	if( NULL == pool )
	{
		perror( "faileded to create memory pool" );
		close( sockfd );
		return ENOMEM;
	}

	writeq = tsfifo_create( FIFO_DEPTH );
	if( NULL == writeq )
	{
		perror( "faileded to create write queue" );
		mempool_destroy( pool );
		close( sockfd );
		return ENOMEM;
	}


	/*
	 * initialize write thread
	 */

	writestat.pool		= pool;
	writestat.writeq	= writeq;
	writestat.bytes		= 0;
	ret = pthread_create( &writestat.tid, NULL, write_thread, &writestat );
	if( ret != 0 )
	{
		fprintf( stderr, "failed to create write thread: %s\n", strerror( ret ) );
		tsfifo_destroy( writeq );
		mempool_destroy( pool );
		close( sockfd );
		return EXIT_FAILURE;
	}


	/*
	 * initialize received threads
	 */

	recvstat.sockfd		= sockfd;
	recvstat.pool		= pool;
	recvstat.writeq		= writeq;
	recvstat.chan_idx	= chan_idx;
	recvstat.wait_pps	= wait_pps;
	recvstat.pkt_lost	= 0;
	recvstat.bytes		= 0;
	ret = pthread_create( &recvstat.tid, NULL, receive_thread, &recvstat );
	if( ret != 0 )
	{
		fprintf( stderr, "failed to create receive thread: %s\n", strerror( ret ) );
		pthread_cancel( recvstat.tid );
		tsfifo_destroy( writeq );
		mempool_destroy( pool );
		close( sockfd );
		return EXIT_FAILURE;
	}


	/*
	 * register termination function
	 */

	signal( SIGINT, stop_processing );


	/*
	 * show statistics
	 */

	uint64_t bytes_recv, bytes_written;
	uint64_t last_bytes_recv = 0, last_bytes_written = 0;
	uint64_t total_lost;

	while( !g_finished )
	{
		bytes_recv		= recvstat.bytes;
		bytes_written	= writestat.bytes;
		total_lost		= recvstat.pkt_lost;

		fprintf( stderr, "\rrecv: %sB/s  ",  dynamic_tgmk(bytes_recv - last_bytes_recv) );
		fprintf( stderr, "write: %sB/s  ",  dynamic_tgmk(bytes_written - last_bytes_written) );
		fprintf( stderr, "recv: %"PRIu64 "  written: %"PRIu64"  pkt_lost: %"PRIu64,
				 bytes_recv, bytes_written, total_lost );

		last_bytes_recv		= bytes_recv;
		last_bytes_written	= bytes_written;

		usleep( 1000 * 1000 );
	}


	/*
	 * wait threads to complete
	 */

	ret = pthread_join( recvstat.tid, &p_exitcode );
	if( ret != 0 )
		fprintf( stderr, "failed to join receive thread: %s\n", strerror( ret ) );

	ret = pthread_join( writestat.tid, &p_exitcode );
	if( ret != 0 )
		fprintf( stderr, "failed to join write thread: %s\n", strerror( ret ) );


	/*
	 * free allocated resources
	 */
	tsfifo_destroy( writeq );
	mempool_destroy( pool );
	close( sockfd );

	return EXIT_SUCCESS;
}

/* vim: set ai nowrap ts=4 sw=4: */
