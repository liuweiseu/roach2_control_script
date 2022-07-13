#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#define    closesocket        close
#endif
#include <sys/time.h>


// #define SWAP_SN     1
//#define VERIFY_SN   1
#define WRITEOUT    1
// #define SEPERATOR   1


const unsigned int BUFFER_SIZE = 63 * 1024;


static inline int64_t mdate( void )
{
    struct timeval tv_date;
    gettimeofday( &tv_date, NULL );
    return ( (int64_t)tv_date.tv_sec * 1000000 + (int64_t)tv_date.tv_usec );
}


static inline uint64_t bswap64( uint8_t * ptr )
{
    uint64_t u64;
    u64 = ((uint64_t)ntohl( *(uint32_t*)ptr ) << 32) |
           (uint64_t)ntohl( *(uint32_t*)(ptr + sizeof(uint32_t)) );
    return u64;
}


/*
 * create socket and bind to local ip/port pair
 */
static int bind_to_local( char * local_ip, char * local_port )
{
    int ret, sockfd = -1;
    struct addrinfo hints, *servaddr, *p;

    memset( &hints, 0, sizeof(hints) );
    hints.ai_flags        = AI_PASSIVE;
    hints.ai_family        = AF_UNSPEC;
    hints.ai_socktype    = SOCK_DGRAM;
    hints.ai_protocol    = IPPROTO_UDP;

    ret = getaddrinfo( local_ip, local_port, &hints, &servaddr );
    if( ret != 0 )
    {
        fprintf( stderr, "getaddrinfo(): %s\n", gai_strerror(ret) );
        return -1;
    }

    for( p = servaddr; p != NULL; p = p->ai_next )
    {
        sockfd = socket( p->ai_family, p->ai_socktype, p->ai_protocol );
        if( sockfd != -1 )
        {
            break;
        }
    }

    if( NULL == p )
    {
        fprintf( stderr, "failed to create socket\n" );
        freeaddrinfo( servaddr );
        return -1;
    }

    ret = bind( sockfd, p->ai_addr, p->ai_addrlen );
    if( ret == -1 )
    {
        fprintf( stderr, "bind(): %s\n", strerror( errno ) );
        closesocket( sockfd );
        freeaddrinfo( servaddr );
#ifdef _WIN32
        WSACleanup();
#endif /* _WIN32 */
        return -1;
    }

    freeaddrinfo( servaddr );

    return sockfd;
}


/*
 * connect to remote ip/port pair if given on command line
 */
static int connect_to_remote( int sockfd, char * remote_host, char * remote_port )
{
    int ret;
    struct addrinfo hints, *servaddr, *p;

    memset( &hints, 0, sizeof(hints) );
    hints.ai_family     = AF_UNSPEC;
    hints.ai_socktype   = SOCK_DGRAM;
    hints.ai_protocol   = IPPROTO_UDP;

    ret = getaddrinfo( remote_host, remote_port, &hints, &servaddr );
    if( ret != 0 )
    {
        fprintf( stderr, "getaddrinfo(): %s\n", gai_strerror(ret) );
        closesocket( sockfd );
        freeaddrinfo( servaddr );
#ifdef _WIN32
        WSACleanup();
#endif /* _WIN32 */
        exit( EXIT_FAILURE );
    }

    for( p = servaddr; p != NULL; p = p->ai_next )
    {
        ret = connect( sockfd, p->ai_addr, p->ai_addrlen );
        if( ret == 0 )
            break;
    }

    if( NULL == p )
    {
        fprintf( stderr, "connect(): %s\n", strerror( errno ) );
        freeaddrinfo( servaddr );
        closesocket( sockfd );
#ifdef _WIN32
        WSACleanup();
#endif /* _WIN32 */
        exit( EXIT_FAILURE );
    }

    freeaddrinfo( servaddr );

    return ret;
}


static int is_multicast_address( char * addr )
{
    struct in_addr in;

    if( inet_aton( addr, &in ) == 0 )
    {
        fprintf( stderr, "Invalid address %s\n", addr );
        return 0;
    }

    return IN_MULTICAST( ntohl( in.s_addr ) ) != 0;
}


static int join_multicast_group( char * mcast_grp, char * mcast_port, char * local_ip )
{
    int                 sockfd;
    struct sockaddr_in  addr;
    struct ip_mreq      mreq;

    fprintf( stderr, "Join multicast group %s:%s from local interface %s ... ", mcast_grp, mcast_port, local_ip );

    if( inet_aton( mcast_grp, &mreq.imr_multiaddr) == 0 )
    {
        fprintf( stderr, "Wrong IPv4 address: %s\n", mcast_grp );
        return -1;
    }

    if( inet_aton( local_ip, &mreq.imr_interface) == 0 )
    {
        fprintf( stderr, "Wrong IPv4 address: %s\n", local_ip );
        return -1;
    }

    sockfd = socket( AF_INET, SOCK_DGRAM, 0 );  // IPPROTO_UDP
    if( sockfd == -1 )
    {
        perror( "socket()" );
        return -1;
    }

    memset( &addr, 0, sizeof(addr) );
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = mreq.imr_multiaddr.s_addr;
    addr.sin_port           = htons( atoi( mcast_port ) );
    if( bind( sockfd, &addr, sizeof(addr) ) == -1 )
    {
        perror( "bind()" );
        closesocket( sockfd );
        return -1;
    }

    if( setsockopt( sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq) ) != 0 )
    {
        perror( "setsockopt()" );
        closesocket( sockfd );
        return -1;
    }

	fprintf( stderr, "done.\n" );

    return sockfd;
}


int main( int argc, char *argv[] )
{
    int             sockfd = -1, optval;
    char            * remote_host, * remote_port, * local_ip, * local_port;
    uint64_t        total_bytes = 0, one_second_bytes = 0, last_sn = UINT64_MAX, pkt_lost = 0;
    unsigned char   * buffer;
    int64_t         start, duration, prev_duration = 0;

    if( argc < 3 )
    {
        fprintf( stderr, "\nUsage: %s <local_ip> <local_port> [remote_host [remote_port]]\n\n", argv[0] );
        return 0;
    }

#ifdef _WIN32
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        printf("WSAStartup failed with error: %d\n", ret);
        return 1;
    }
#endif /* _WIN32 */

    local_ip    = argv[1];
    local_port  = argv[2];
    remote_host = (argc < 4) ? NULL : argv[3];
    remote_port = (argc < 5) ? NULL : argv[4];

    if( remote_host != NULL && is_multicast_address( remote_host ) )
    {
        sockfd = join_multicast_group( remote_host, local_port, local_ip );
        if( sockfd == -1 )
            exit( EXIT_FAILURE );
    }
    else
    {
        sockfd = bind_to_local( local_ip, local_port );
        if( sockfd == -1 )
            exit( EXIT_FAILURE );

        if( remote_host != NULL )
        {
            if( connect_to_remote( sockfd, remote_host, remote_port ) == -1 )
                exit( EXIT_FAILURE );
        }
    }

	optval = 50;
	if( setsockopt(sockfd, SOL_SOCKET, SO_BUSY_POLL, &optval, sizeof(optval)) != 0 )
		perror( "SO_BUSY_POOL not supported" );

    buffer = (unsigned char *)malloc( BUFFER_SIZE );
    if( NULL == buffer )
    {
        fprintf( stderr, "insufficient memory\n" );
        closesocket( sockfd );
#ifdef _WIN32
        WSACleanup();
#endif /* _WIN32 */
        return -1;
    }

    start = mdate( );

    while( 1 )
    {
        ssize_t    recv_len;

        recv_len = recv( sockfd, buffer, BUFFER_SIZE, 0 );

        if( recv_len == -1 )
        {
            fprintf( stderr, "\nrecv(): %s\n", strerror( errno ) );
        }
        else if( recv_len == 0 )
        {
            fprintf( stderr, "\npeer shutdown?!\n\n" );
            break;
        }
        else
        {
            duration = mdate( ) - start;
            if( duration <= 0 )
                duration = 1;

            total_bytes += recv_len;
            one_second_bytes += recv_len;

#ifdef VERIFY_SN
            /* check serial number only if we already received 1 packet */
#ifdef SWAP_SN
            register uint64_t sn = bswap64(buffer);
#else
            register uint64_t sn = *(uint64_t *)buffer;
#endif /* SWAP_SN */
            if( last_sn != UINT64_MAX )
            {
                if( last_sn + 1 != sn )
                {
                    pkt_lost += sn - last_sn - 1;
                    //fprintf( stderr, "%" PRIu64 " lost after %" PRIx64 "\n", sn - last_sn - 1, last_sn );
                }
            }
            last_sn = sn;
#endif /* VERIFY_SN */

#ifdef WRITEOUT
            /* put received bytes to stdout */
            if( fwrite( buffer, recv_len, 1, stdout ) != 1 )
                fprintf( stderr, "\nError writing packet" );

#ifdef SEPERATOR
			/* seperator between packets */
			fwrite( "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8, 1, stdout );
#endif /* SPEERATOR */
#endif /* WRITEOUT */

            /* Show statistics every second */
            if( duration - prev_duration >= 1000000 )
            {
                uint64_t    bps;

                //bps = total_bytes * 1000000 / duration;
                bps = one_second_bytes * 1000000 / (duration - prev_duration);
                fprintf( stderr, "\rreceived bytes: %" PRIu64 ", ", total_bytes );
                if( bps > 1000 * 1000 * 1000 )
                {
                    fprintf( stderr, "%.3f GB/s", (float)bps / 1000 / 1000 / 1000 );
                }
                else if( bps > 1000 * 1000 )
                {
                    fprintf( stderr, "%.3f MB/s", (float)bps / 1000 / 1000 );
                }
                else if( bps > 1000 )
                {
                    fprintf( stderr, "%.3f KB/s", (float)bps / 1000 );
                }
                else
                {
                    fprintf( stderr, "%" PRIu64 "B/s", bps );
                }
                prev_duration = duration;
                one_second_bytes = 0;
#ifdef VERIFY_SN
                if( pkt_lost > 0 )
                    fprintf( stderr, "  %" PRIu64 " packets lost", pkt_lost );
#endif /* VERIFY_SN */
            }
        }
    }

    closesocket( sockfd );

#ifdef _WIN32
    WSACleanup();
#endif /* _WIN32 */

    return 0;
}
