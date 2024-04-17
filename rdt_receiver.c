#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"

#define BUFFER_SIZE 1024
#define PORT 12345

int sockfd;
struct sockaddr_in serveraddr, clientaddr;
socklen_t clientlen;
tcp_packet *recv_buffer[BUFFER_SIZE];
int NextPacketExpected = 0;


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;

void initialize_recv_buffer() {
    for (int i = 0; i < BUFFER_SIZE; i++) {
        recv_buffer[i] = NULL;
    }
}

void send_ack(int ackno) {
    tcp_packet *ack_pkt = make_packet(0);
    ack_pkt->hdr.ackno = ackno;
    ack_pkt->hdr.ctr_flags = ACK;

    if (sendto(sockfd, ack_pkt, sizeof(tcp_packet), 0,
               (struct sockaddr *)&clientaddr, clientlen) < 0) {
        perror("sendto");
    }
    free(ack_pkt);
}

void process_packet(tcp_packet *pkt) {
    int seqno = pkt->hdr.seqno;
    if (seqno >= NextPacketExpected && seqno < NextPacketExpected + BUFFER_SIZE) {
        // Place packet in buffer relative to the expected sequence number
        int idx = seqno % BUFFER_SIZE;
        if (recv_buffer[idx] == NULL) {  // Only save if slot is empty (no duplicate packets)
            recv_buffer[idx] = pkt;
        } else {
            free(pkt);  // Free packet if slot was not empty (duplicate packet received)
        }

        // Update NextPacketExpected and send cumulative ACK if packets are in sequence
        while (recv_buffer[NextPacketExpected % BUFFER_SIZE] != NULL) {
            tcp_packet *ready_pkt = recv_buffer[NextPacketExpected % BUFFER_SIZE];
            // Process the packet (e.g., assemble data, execute commands, etc.)

            printf("Processed packet with SEQNO %d\n", ready_pkt->hdr.seqno);

            // Free the buffer slot
            free(ready_pkt);
            recv_buffer[NextPacketExpected % BUFFER_SIZE] = NULL;

            // Increment to the next expected packet
            NextPacketExpected++;
        }

        // Send cumulative ACK for the highest in-sequence packet received
        send_ack(NextPacketExpected);
    } else {
        free(pkt);  // Free the packet if it's outside the buffer window
    }
}

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    initialize_recv_buffer();
    clientlen = sizeof(clientaddr);

    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        //VLOG(DEBUG, "waiting from server \n");
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        if ( recvpkt->hdr.data_size == 0) {
            //VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            break;
        }
        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

        fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
        fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;
        sndpkt->hdr.ctr_flags = ACK;
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }

        tcp_packet *pkt = (tcp_packet *) malloc(sizeof(tcp_packet));
        if (recvfrom(sockfd, pkt, sizeof(tcp_packet), 0, 
                     (struct sockaddr *) &clientaddr, &clientlen) < 0) {
            error("recvfrom");
        }

        process_packet(pkt);
    }

    close(sockfd);
    return 0;
}
