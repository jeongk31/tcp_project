#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond
#define WINDOW_SIZE 10

int next_seqno=0;
int send_base=0;
int window_size = WINDOW_SIZE;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       

tcp_packet *send_buffer[WINDOW_SIZE];
int window_base = 0;
int next_packet_to_send = 0;
int packets_in_buffer = 0;

int last_acked_seqno = -1;
int duplicate_ack_count = 0;

int all_data_sent = 0;
int maxAck = -1;

void send_packets_within_window() {
    for (int i = 0; i < WINDOW_SIZE; i++) {
        int idx = (window_base + i) % WINDOW_SIZE;
        if (send_buffer[idx] != NULL && next_packet_to_send <= window_base + WINDOW_SIZE) {
            if (sendto(sockfd, send_buffer[idx], sizeof(tcp_packet) + send_buffer[idx]->hdr.data_size, 0, 
                      (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("ERROR in sendto");
            }
            if (next_packet_to_send == window_base) { // Start timer when the first packet is sent
                start_timer();
            }
            next_packet_to_send++;
        }
    }
}

void ack_received(int ackno) {
    if (ackno == last_acked_seqno) {
        // If this ACK is a duplicate
        duplicate_ack_count++;
        if (duplicate_ack_count == 3) {
            // Fast retransmit on three duplicate ACKs
            VLOG(INFO, "Three duplicate ACKs received, fast retransmitting");
            int idx = window_base % WINDOW_SIZE;
            if (send_buffer[idx] != NULL) {
                if (sendto(sockfd, send_buffer[idx], sizeof(tcp_packet) + send_buffer[idx]->hdr.data_size, 0, 
                          (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
                start_timer(); // Restart the timer after fast retransmission
            }
        }
    } else if (ackno > last_acked_seqno) {
        // If this ACK acknowledges new data
        duplicate_ack_count = 0;  // Reset duplicate ACK counter
        last_acked_seqno = ackno;  // Update the last acknowledged sequence number

        // Acknowledge all packets up to ackno
        while (window_base <= ackno) {
            int idx = window_base % WINDOW_SIZE;
            if (send_buffer[idx] != NULL) {
                free(send_buffer[idx]);  // Free the memory used by the packet
                send_buffer[idx] = NULL; // Clear the buffer slot
            }
            window_base++;  // Move the window base forward
        }

        if (window_base > next_packet_to_send) {
            window_base = next_packet_to_send;
        }
        
        if (window_base == next_packet_to_send) {
            stop_timer();  // Stop the timer if all packets are acknowledged
            if (all_data_sent) {
                VLOG(INFO, "All data transmitted and acknowledged.");
                close(sockfd); // Close the socket
                exit(0); // Exit cleanly
            }
        } else {
            start_timer(); // Otherwise, restart the timer for the remaining packets
        }
    }
}

void initialize_send_buffer() {
    for(int i = 0; i < WINDOW_SIZE; i++) {
        send_buffer[i] = NULL;
    }
}

void resend_packets(int sig) {
    if (sig == SIGALRM) {
        VLOG(INFO, "Timeout happened, resending unacknowledged packets from window_base");
        for (int i = 0; i < (next_packet_to_send - window_base); i++) {
            int idx = (window_base + i) % WINDOW_SIZE;
            if (send_buffer[idx] != NULL) {
                if (sendto(sockfd, send_buffer[idx], sizeof(tcp_packet) + send_buffer[idx]->hdr.data_size, 0, 
                          (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            }
        }
        start_timer(); // Restart the timer after resending
    }
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    // Stop and wait protocol

    init_timer(RETRY, resend_packets);
    next_seqno = 0;

    initialize_send_buffer();

    while (1)
    {
        len = fread(buffer, 1, DATA_SIZE, fp);
        if (len <= 0)
        {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sndpkt->hdr.seqno = next_seqno;
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            all_data_sent = 1;
            break;
        }
        send_base = next_seqno;
        next_seqno = send_base + len;
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = send_base;

        int buffer_index = send_base % WINDOW_SIZE;
        if (send_buffer[buffer_index] != NULL) {
            free(send_buffer[buffer_index]);
        }
        send_buffer[buffer_index] = sndpkt;
        
        next_packet_to_send++;

        // Wait for ACK
        do {
            VLOG(DEBUG, "Sending packet %d to %s", 
                    send_base, inet_ntoa(serveraddr.sin_addr));
            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            start_timer();
            // ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
            // struct sockaddr *src_addr, socklen_t *addrlen);
            do
            {
                if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                            (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
                {
                    error("recvfrom");
                }

                recvpkt = (tcp_packet *)buffer;
                int received_ack = recvpkt->hdr.ackno;
                ack_received(received_ack);

                if (recvpkt->hdr.ackno >= window_base) {
                    ack_received(recvpkt->hdr.ackno);
                    if (all_data_sent && window_base == next_packet_to_send) {
                        // All data has been sent and all sent packets have been acknowledged
                        VLOG(INFO, "All data transmitted and acknowledged.");
                        close(sockfd); // Close the socket
                        exit(0); // Exit cleanly
                    }
                }
                printf("%d \n", get_data_size(recvpkt));
                assert(get_data_size(recvpkt) <= DATA_SIZE);
            } while (recvpkt->hdr.ackno < next_seqno);    // ignore duplicate ACKs
            stop_timer();
            /* resend pack if don't recv ACK */
        } while (recvpkt->hdr.ackno != next_seqno);      
    
        free(sndpkt);
    }

    send_packets_within_window();

    return 0;
}



