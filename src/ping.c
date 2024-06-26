#ifndef _GNU_SOURCE
    #define _GNU_SOURCE /* for additional type definitions */
#endif

#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601 /* for inet_XtoY functions on MinGW */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>

#ifdef _WIN32

#include <process.h>  /* _getpid() */
#include <winsock2.h>
#include <ws2tcpip.h> /* getaddrinfo() */
#include <mswsock.h>  /* WSARecvMsg() */

#undef CMSG_SPACE
#define CMSG_SPACE WSA_CMSG_SPACE
#undef CMSG_FIRSTHDR
#define CMSG_FIRSTHDR WSA_CMSG_FIRSTHDR
#undef CMSG_NXTHDR
#define CMSG_NXTHDR WSA_CMSG_NXTHDR
#undef CMSG_DATA
#define CMSG_DATA WSA_CMSG_DATA

typedef SOCKET socket_t;
typedef WSAMSG msghdr_t;
typedef WSACMSGHDR cmsghdr_t;

/*
 * Pointer to the WSARecvMsg() function. It must be obtained at runtime...
 */
static LPFN_WSARECVMSG WSARecvMsg;

#else /* _WIN32 */

#ifdef __APPLE__
    #define __APPLE_USE_RFC_3542 /* for IPv6 definitions on Apple platforms */
#endif

#include <errno.h>
#include <fcntl.h>            /* fcntl() */
#include <netdb.h>            /* getaddrinfo() */
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>        /* inet_XtoY() */
#include <netinet/in.h>       /* IPPROTO_ICMP */
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>  /* struct icmp */
//#include <netinet/icmp6.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "cping.h"

typedef int socket_t;
typedef struct msghdr msghdr_t;
typedef struct cmsghdr cmsghdr_t;

#endif /* !_WIN32 */

#define IP_VERSION_ANY 0
#define IP_V4 4
#define IP_V6 6

#define ICMP_HEADER_LENGTH 8
#define MESSAGE_BUFFER_SIZE 1024
#define ICMP_PAYLOAD_SIZE 32  // Define the size of the ICMP payload buffer

#ifndef ICMP_ECHO
    #define ICMP_ECHO 8
#endif
#ifndef ICMP_ECHO6
    #define ICMP6_ECHO 128
#endif
#ifndef ICMP_ECHO_REPLY
    #define ICMP_ECHO_REPLY 0
#endif
#ifndef ICMP_ECHO_REPLY6
    #define ICMP6_ECHO_REPLY 129
#endif

#define REQUEST_TIMEOUT 1000000  //microsecond, us => 1sec
#define REQUEST_INTERVAL 1000000  //microsecond, us => 1sec

#ifdef _WIN32
    #define socket(af, type, protocol) \
        WSASocketW(af, type, protocol, NULL, 0, 0)
    #define close_socket closesocket
    #define getpid _getpid
    #define usleep(usec) Sleep((DWORD)((usec) / 1000))
#else
    #define close_socket close
#endif

#pragma pack(push, 1)

#if defined _WIN32 || defined __CYGWIN__

#if defined _MSC_VER || defined __MINGW32__
    typedef unsigned __int8 uint8_t;
    typedef unsigned __int16 uint16_t;
    typedef unsigned __int32 uint32_t;
    typedef unsigned __int64 uint64_t;
    #ifndef EAI_SYSTEM
        #define EAI_SYSTEM	  -11
    #endif
#endif

struct icmp {
    uint8_t icmp_type;
    uint8_t icmp_code;
    uint16_t icmp_cksum;
    uint16_t icmp_id;
    uint16_t icmp_seq;
};

#endif /* _WIN32 || __CYGWIN__ */

struct ip6_pseudo_hdr {
    struct in6_addr src;
    struct in6_addr dst;
    uint8_t unused1[2];
    uint16_t plen;
    uint8_t unused2[3];
    uint8_t nxt;
};

struct icmp6_packet {
    struct ip6_pseudo_hdr ip6_hdr;
    struct icmp icmp;
};

#pragma pack(pop)

#ifdef _WIN32

/**
 * psockerror() is like perror() but for the Windows Sockets API.
 */
static void psockerror(const char *s)
{
    char *message = NULL;
    DWORD format_flags = FORMAT_MESSAGE_FROM_SYSTEM
        | FORMAT_MESSAGE_IGNORE_INSERTS
        | FORMAT_MESSAGE_ALLOCATE_BUFFER
        | FORMAT_MESSAGE_MAX_WIDTH_MASK;
    DWORD result;

    result = FormatMessageA(format_flags,
                            NULL,
                            WSAGetLastError(),
                            0,
                            (char *)&message,
                            0,
                            NULL);
    if (result > 0) {
        fprintf(stderr, "%s: %s\n", s, message);
        LocalFree(message);
    } else {
        fprintf(stderr, "%s: Unknown error\n", s);
    }
}

#else /* _WIN32 */

#define psockerror perror

#endif /* !_WIN32 */

/**
 * Returns a timestamp with microsecond resolution.
 */
static uint64_t utime(void)
{
#ifdef _WIN32
    LARGE_INTEGER count;
    LARGE_INTEGER frequency;
    if (QueryPerformanceCounter(&count) == 0
        || QueryPerformanceFrequency(&frequency) == 0) {
        return 0;
    }
    return count.QuadPart * 1000000 / frequency.QuadPart;
#else
    struct timeval now;
    return gettimeofday(&now, NULL) != 0
        ? 0
        : now.tv_sec * 1000000 + now.tv_usec;
#endif
}

#ifdef _WIN32

static void init_winsock_lib(void)
{
    int error;
    WSADATA wsa_data;

    error = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (error != 0) {
        fprintf(stderr, "Failed to initialize WinSock: %d\n", error);
        exit(EXIT_FAILURE);
    }
}

static void init_winsock_extensions(socket_t sockfd)
{
    int error;
    GUID recvmsg_id = WSAID_WSARECVMSG;
    DWORD size;

    /*
     * Obtain a pointer to the WSARecvMsg (recvmsg) function.
     */
    error = WSAIoctl(sockfd,
                     SIO_GET_EXTENSION_FUNCTION_POINTER,
                     &recvmsg_id,
                     sizeof(recvmsg_id),
                     &WSARecvMsg,
                     sizeof(WSARecvMsg),
                     &size,
                     NULL,
                     NULL);
    if (error == SOCKET_ERROR) {
        psockerror("WSAIoctl");
        exit(EXIT_FAILURE);
    }
}

#endif /* _WIN32 */

static uint16_t compute_checksum(const char *buf, size_t size)
{
    /* RFC 1071 - http://tools.ietf.org/html/rfc1071 */

    size_t i;
    uint64_t sum = 0;

    for (i = 0; i < size; i += 2) {
        sum += *(uint16_t *)buf;
        buf += 2;
    }
    if (size - i > 0)
        sum += *(uint8_t *)buf;

    while ((sum >> 16) != 0)
        sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)~sum;
}

void current_time(char *timestempformat) {
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];

    time(&rawtime);
    timeinfo = localtime(&rawtime);

    strftime(buffer, sizeof(buffer), "%Y%m%d_%H:%M:%S ", timeinfo);
    //strftime(buffer, sizeof(buffer), &timestempformat, timeinfo);
    printf("%s", buffer);
}

void help(char **argv){
    //printf("Usage: %s [-4] [-6] [-n num] [-l size] [-S srcaddr] [-t[format]] hostname\n", argv[0]);
    printf("Usage: %s [-4] [-6] [-n num] [-l size] [-t[format]] hostname\n", argv[0]);
    printf("\t [-n num]     Number of echo requests to send (without this option, it will ping continue)\n");
    printf("\t [-l size]     Send buffer size\n");
    //printf("\t [-S srcaddr]     Source address to use\n");
    printf("\t [-4]     Force using IPv4\n");
    printf("\t [-6]     Force using IPv6\n");
    printf("\t [-t]     show timestemp, default format: '%%Y%%m%%d_%%H:%%M:%%S'\n");
}

int main(int argc, char **argv)
{
    int i;
    char *hostname = NULL;
    // char *srcaddr = NULL;
    int ip_version = IP_VERSION_ANY;
    // Allocate space for the ICMP payload buffer
    // char icmp_payload[ICMP_PAYLOAD_SIZE];
    int icmp_payload_size=ICMP_PAYLOAD_SIZE;
    int showtimestemp = 0;
    char *timestempformat = NULL;
    int error;
    socket_t sockfd = -1;
    struct addrinfo *addrinfo_list = NULL;
    struct addrinfo *addrinfo;
    char addr_str[INET6_ADDRSTRLEN] = "<unknown>";
    struct sockaddr_storage addr;
    socklen_t dst_addr_len;
    uint16_t id = (uint16_t)getpid();
    uint16_t seq;
    uint64_t start_time;
    uint64_t delay;
    int opt;
    int max_num=0; // number of echo request
    
    static struct option long_options[] = {
        {"num", no_argument, 0, 'n'},
        {"size", no_argument, 0, 'l'},
        //{"srcaddr", no_argument, 0, 'S'},
        {"ipv4", no_argument, 0, '4'},
        {"ipv6", no_argument, 0, '6'},
        {"hostname", required_argument, 0, 'h'},
        {"timestemp", required_argument, 0, 't'},
        {"version", required_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

// Parse command-line options
    //while ((opt = getopt(argc, argv, "46ht::")) != -1) {
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, "vn:l:46ht::", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'n': //num of echo request
                max_num = atoi(optarg);
                break;
            case 'l':
                icmp_payload_size = atoi(optarg);
                break;
            // case 'S':
            //     srcaddr = optarg;
            //     printf("srcaddr : %s\n",  srcaddr);
            //     break;
            case 't':
                showtimestemp=1;
                timestempformat = optarg;
                if (timestempformat== NULL){
                    //# default format
                    timestempformat="%Y-%m-%d_%H:%M:%S";
                }
                break;
            case '4':
                ip_version = IP_V4;
                break;
            case '6':
                ip_version = IP_V6;
                break;
            case 'h':
                // Print usage information
                help(argv);
                return 0;
            case 'v':
                printf("%s\n", VER_FILEVERSION_STR);
                return 0;
            case '?':
                // Invalid option
                help(argv);
                return 1;
            default:
                help(argv);
        }
    }
    // Process non-option arguments
    for (; optind < argc; optind++) {
        // Assuming only one hostname argument is expected
        if (hostname == NULL) {
            hostname = argv[optind];
        } else {
            fprintf(stderr, "Error: Only one hostname argument is expected.\n");
            return 1;
        }
    }

    if (hostname == NULL) {
        help(argv);
        goto exit_error;
    }

#ifdef _WIN32
    init_winsock_lib();
#endif

    if (ip_version == IP_V4 || ip_version == IP_VERSION_ANY) {
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_RAW;
        hints.ai_protocol = IPPROTO_ICMP;
        error = getaddrinfo(hostname,
                            NULL,
                            &hints,
                            &addrinfo_list);
    }
    if (ip_version == IP_V6
        || (ip_version == IP_VERSION_ANY && error != 0)) {
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_RAW;
        hints.ai_protocol = IPPROTO_ICMPV6;
        error = getaddrinfo(hostname,
                            NULL,
                            &hints,
                            &addrinfo_list);
    }
    if (error != 0) {
        if (error == EAI_SYSTEM){
            fprintf(stderr, "getaddrinfo: %s\n", strerror(errno));
        }else{
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        }
        goto exit_error;
    }

    for (addrinfo = addrinfo_list;
        addrinfo != NULL;
        addrinfo = addrinfo->ai_next) {
        sockfd = socket(addrinfo->ai_family,
                        addrinfo->ai_socktype,
                        addrinfo->ai_protocol);
        if (sockfd >= 0) {
            break;
        }
    }
    if ((int)sockfd < 0) {
        psockerror("socket");
        goto exit_error;
    }
    // char icmp_payload[icmp_payload_size];
    // Allocate space for the ICMP payload buffer
    char *icmp_payload = (char *)malloc(icmp_payload_size);
    // Fill the ICMP payload buffer with some data (if needed)
    // For example, you might fill it with zeros or some specific data
    memset(icmp_payload, 255, icmp_payload_size);

    memcpy(&addr, addrinfo->ai_addr, addrinfo->ai_addrlen);
    dst_addr_len = (socklen_t)addrinfo->ai_addrlen;

    freeaddrinfo(addrinfo_list);
    addrinfo = NULL;
    addrinfo_list = NULL;

#ifdef _WIN32
    init_winsock_extensions(sockfd);
#endif

    /*
     * Switch the socket to non-blocking I/O mode. This allows us to implement
     * the timeout feature.
     */
#ifdef _WIN32
    {
        u_long opt_value = 1;
        if (ioctlsocket(sockfd, FIONBIO, &opt_value) != 0) {
            psockerror("ioctlsocket");
            goto exit_error;
        }
    }
#else /* _WIN32 */
    if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1) {
        psockerror("fcntl");
        goto exit_error;
    }
#endif /* !_WIN32 */

    if (addr.ss_family == AF_INET6) {
        /*
         * This allows us to receive IPv6 packet headers in incoming messages.
         */
        int opt_value = 1;
        error = setsockopt(sockfd,
                           IPPROTO_IPV6,
#if defined _WIN32 || defined __CYGWIN__
                           IPV6_PKTINFO,
#else
                           IPV6_RECVPKTINFO,
#endif
                           (char *)&opt_value,
                           sizeof(opt_value));
        if (error != 0) {
            psockerror("setsockopt");
            goto exit_error;
        }
    }

    /*
     * As opening raw sockets usually requires superuser privileges, we should
     * drop them as soon as possible for security reasons.
     */
#if !defined _WIN32
    /* Note: group ID must be set before user ID! */
    if (setgid(getgid() != 0)) {
        perror("setgid");
        goto exit_error;
    }
    if (setuid(getuid()) != 0) {
        perror("setuid");
        goto exit_error;
    }
#endif

    /*
     * Convert the destination IP-address to a string.
     */
    inet_ntop(addr.ss_family,
              addr.ss_family == AF_INET6
                  ? (void *)&((struct sockaddr_in6 *)&addr)->sin6_addr
                  : (void *)&((struct sockaddr_in *)&addr)->sin_addr,
              addr_str,
              sizeof(addr_str));

    printf("Pinging %s (%s)\n", hostname, addr_str);
    fflush(stdout);
    for (seq = 0; ; seq++) {
        if (max_num>0){
            if (seq>=max_num){
                break;
            }
        }
        
        struct icmp request;

        request.icmp_type =
                addr.ss_family == AF_INET6 ? ICMP6_ECHO : ICMP_ECHO;
        request.icmp_code = 0;
        request.icmp_cksum = 0;
        request.icmp_id = htons(id);
        request.icmp_seq = htons(seq);
#if !defined _WIN32
    //TODO: mingw64 how to add payload?
        // Copy the ICMP payload into the request packet
        memcpy(request.icmp_data, icmp_payload, icmp_payload_size);
#endif
        
    //    // Allocate space for the ICMP packet
    //    unsigned long icmphdr_size;
    //     if (addr.ss_family == AF_INET6) {
    //         icmphdr_size = sizeof(struct icmp6_hdr);
    //     } else{
    //         icmphdr_size = sizeof(struct icmphdr);
    //     }
    //     char icmp_packet[icmp_payload_size + icmphdr_size];
    //     if (addr.ss_family == AF_INET6) {
    //         struct icmp6_hdr *request = (struct icmp6_hdr *)icmp_packet;
    //         // Fill the ICMPv6 header
    //         request->icmp6_type = ICMP6_ECHO;
    //         request->icmp6_code = 0;
    //         request->icmp6_cksum = 0;
    //         request->icmp6_dataun.icmp6_un_data16[0] = htons(id);
    //         request->icmp6_dataun.icmp6_un_data16[1] = htons(seq);
    //     } else {
    //         struct icmphdr *request = (struct icmphdr *)icmp_packet;
    //         // Fill the ICMP header
    //         request->type = ICMP_ECHO;
    //         request->code = 0;
    //         request->checksum = 0;
    //         request->un.echo.id = htons(id);
    //         request->un.echo.sequence = htons(seq);
    //     }
    //     // Fill the ICMP payload with some data (if needed)
    //     // For example, you might fill it with zeros or some specific data
    //     memset(icmp_packet + icmphdr_size, 0, icmp_payload_size);

        if (addr.ss_family == AF_INET6) {
            /*
             * Checksum is calculated from the ICMPv6 packet prepended
             * with an IPv6 "pseudo-header".
             *
             * https://tools.ietf.org/html/rfc2463#section-2.3
             * https://tools.ietf.org/html/rfc2460#section-8.1
             */
            struct icmp6_packet request_packet = {0};

            request_packet.ip6_hdr.src = in6addr_loopback;
            request_packet.ip6_hdr.dst =
                ((struct sockaddr_in6 *)&addr)->sin6_addr;
            request_packet.ip6_hdr.plen = htons((uint16_t)ICMP_HEADER_LENGTH);
            request_packet.ip6_hdr.nxt = IPPROTO_ICMPV6;
            request_packet.icmp = request;

            request.icmp_cksum = compute_checksum((char *)&request_packet,
                                                   sizeof(request_packet));
            // request->checksum = compute_checksum((char *)&request_packet,
            //                                       sizeof(request_packet));                                                  
        } else {
            request.icmp_cksum = compute_checksum((char *)&request,
                                                  sizeof(request)+ icmp_payload_size);
            // request->checksum = compute_checksum((char *)&request,
            //                                       sizeof(request)+ icmp_payload_size);
        }
        
        // if (srcaddr != NULL) {
        //     // Initialize sockaddr_in structure
        //     int local_port = 12345; // Example port number
        //     struct sockaddr_in local_addr;
        //     memset(&local_addr, 0, sizeof(local_addr));
        //     local_addr.sin_family = AF_INET;
        //     local_addr.sin_addr.s_addr = inet_addr(srcaddr);
        //     local_addr.sin_port = htons(local_port);
        //     // Bind the socket to a specific local address (source address)
        //     if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        //         perror("bind");
        //         // Error handling if binding fails
        //         goto exit_error;
        //     }
        // }
        error = (int)sendto(sockfd,
                            (char *)&request,
                            sizeof(request)+ icmp_payload_size,
                            0,
                            (struct sockaddr *)&addr,
                            (int)dst_addr_len);
        if (error < 0) {
            psockerror("sendto");
            goto exit_error;
        }

        start_time = utime();

        for (;;) {
            char msg_buf[MESSAGE_BUFFER_SIZE];
            char packet_info_buf[MESSAGE_BUFFER_SIZE];
            struct in6_addr msg_addr = {0};
#ifdef _WIN32
            WSABUF msg_buf_struct = {
                sizeof(msg_buf),
                msg_buf
            };
            WSAMSG msg = {
                NULL,
                0,
                &msg_buf_struct,
                1,
                {sizeof(packet_info_buf), packet_info_buf},
                0
            };
            DWORD msg_len = 0;
#else /* _WIN32 */
            struct iovec msg_buf_struct = {
                msg_buf,
                sizeof(msg_buf)
            };
            struct msghdr msg = {
                NULL,
                0,
                &msg_buf_struct,
                1,
                packet_info_buf,
                sizeof(packet_info_buf),
                0
            };
            size_t msg_len;
#endif /* !_WIN32 */
            cmsghdr_t *cmsg;
            size_t ip_hdr_len;
            struct icmp *reply;
            int reply_id;
            int reply_seq;
            uint16_t reply_checksum;
            uint16_t checksum;

#ifdef _WIN32
            error = WSARecvMsg(sockfd, &msg, &msg_len, NULL, NULL);
#else
            error = (int)recvmsg(sockfd, &msg, 0);
#endif

            delay = utime() - start_time;

            if (error < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
                if (errno == EAGAIN) {
#endif
                    if (delay > REQUEST_TIMEOUT) {
                        if (showtimestemp){
                            current_time(timestempformat);
                        }
                        printf("Request timed out: seq=%d\n", seq);
                        fflush(stdout);
                        goto next;
                    } else {
                        /* No data available yet, try to receive again. */
                        continue;
                    }
                } else {
                    psockerror("recvmsg");
                    goto next;
                }
            }

#ifndef _WIN32
            msg_len = error;
#endif

            if (addr.ss_family == AF_INET6) {
                /*
                 * The IP header is not included in the message, msg_buf points
                 * directly to the ICMP data.
                 */
                ip_hdr_len = 0;

                /*
                 * Extract the destination address from IPv6 packet info. This
                 * will be used to compute the checksum later.
                 */
                for (
                    cmsg = CMSG_FIRSTHDR(&msg);
                    cmsg != NULL;
                    cmsg = CMSG_NXTHDR(&msg, cmsg))
                {
                    if (cmsg->cmsg_level == IPPROTO_IPV6
                        && cmsg->cmsg_type == IPV6_PKTINFO) {
                        struct in6_pktinfo *pktinfo = (void *)CMSG_DATA(cmsg);
                        memcpy(&msg_addr,
                               &pktinfo->ipi6_addr,
                               sizeof(struct in6_addr));
                    }
                }
            } else {
                /*
                 * For IPv4, we must take the length of the IP header into
                 * account.
                 *
                 * Header length is stored in the lower 4 bits of the VHL field
                 * (VHL = Version + Header Length).
                 */
                ip_hdr_len = ((*(uint8_t *)msg_buf) & 0x0F) * 4;
            }

            reply = (struct icmp *)(msg_buf + ip_hdr_len);
            reply_id = ntohs(reply->icmp_id);
            reply_seq = ntohs(reply->icmp_seq);

            /*
             * Verify that this is indeed an echo reply packet.
             */
            if (!(addr.ss_family == AF_INET
                  && reply->icmp_type == ICMP_ECHO_REPLY)
                && !(addr.ss_family == AF_INET6
                     && reply->icmp_type == ICMP6_ECHO_REPLY)) {
                continue;
            }

            /*
             * Verify the ID and sequence number to make sure that the reply
             * is associated with the current request.
             */
            if (reply_id != id || reply_seq != seq) {
                continue;
            }

            reply_checksum = reply->icmp_cksum;
            reply->icmp_cksum = 0;

            /*
             * Verify the checksum.
             */
            if (addr.ss_family == AF_INET6) {
                size_t size = sizeof(struct ip6_pseudo_hdr) + msg_len;
                struct icmp6_packet *reply_packet = calloc(1, size);

                if (reply_packet == NULL) {
                    psockerror("malloc");
                    goto exit_error;
                }

                memcpy(&reply_packet->ip6_hdr.src,
                       &((struct sockaddr_in6 *)&addr)->sin6_addr,
                       sizeof(struct in6_addr));
                reply_packet->ip6_hdr.dst = msg_addr;
                reply_packet->ip6_hdr.plen = htons((uint16_t)msg_len);
                reply_packet->ip6_hdr.nxt = IPPROTO_ICMPV6;
                memcpy(&reply_packet->icmp,
                       msg_buf + ip_hdr_len,
                       msg_len - ip_hdr_len);

                checksum = compute_checksum((char *)reply_packet, size);
            } else {
                checksum = compute_checksum(msg_buf + ip_hdr_len,
                                            msg_len - ip_hdr_len);
            }
            if (showtimestemp){
                current_time(timestempformat);
            }
            printf("Reply from %s: seq=%d, time=%.3f ms%s\n",
                   addr_str,
                   seq,
                   (double)delay / 1000.0,
                   reply_checksum != checksum ? " (bad checksum)" : "");
            fflush(stdout);
            break;
        }

next:
        if (delay < REQUEST_INTERVAL) {
            usleep(REQUEST_INTERVAL - delay);
        }
    }

    close_socket(sockfd);

    return EXIT_SUCCESS;

exit_error:

    if (addrinfo_list != NULL) {
        freeaddrinfo(addrinfo_list);
    }

    close_socket(sockfd);

    return EXIT_FAILURE;
}
