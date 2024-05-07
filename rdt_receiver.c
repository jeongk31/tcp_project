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
#define MAX_BUFFER_SIZE 50 // Max receiver buffer size

tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet *receiver_buffer_arr[MAX_BUFFER_SIZE];

int main(int argc, char **argv)
{
    int sockfd;                    /* socket */
    int portno;                    /* port to listen on */
    int clientlen;                 /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval;                    /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;
    int subsequent_seqno = 0; // Variable that holds the value for the current required seqno

    // Loop for assigning packets with data_size 0 to the array of the tcp packets
    for (int i = 0; i < MAX_BUFFER_SIZE; i++)
    {
        receiver_buffer_arr[i] = make_packet(MSS_SIZE);
        receiver_buffer_arr[i]->hdr.data_size = 0;
        receiver_buffer_arr[i]->hdr.seqno = -1;
    }

    /*
     * check command line arguments
     */
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp = fopen(argv[2], "w");
    if (fp == NULL)
    {
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
               (const void *)&optval, sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /*
     * bind: associate the parent socket with a port
     */
    if (bind(sockfd, (struct sockaddr *)&serveraddr,
             sizeof(serveraddr)) < 0)
        error("ERROR on binding");

    /*
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1)
    {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        // VLOG(DEBUG, "waiting from server \n");
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                     (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen) < 0)
        {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *)buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        if (recvpkt->hdr.data_size == 0)
        {
            VLOG(INFO, "End Of File has been reached");
            fclose(fp);
            break;
        }

        // Handle out of order packets
        if (subsequent_seqno < recvpkt->hdr.seqno)
        {
            // Check for duplicate packets
            int is_duplicate = 0;
            for (int i = 0; i < MAX_BUFFER_SIZE; i++)
            {
                if (receiver_buffer_arr[i]->hdr.seqno == recvpkt->hdr.seqno)
                {
                    is_duplicate = 1;
                    break;
                }
            }

            // Store packet if not a duplicate
            if (!is_duplicate)
            {
                for (int i = 0; i < MAX_BUFFER_SIZE; i++)
                {
                    if (receiver_buffer_arr[i]->hdr.seqno == -1 && receiver_buffer_arr[i]->hdr.data_size == 0)
                    {
                        memcpy(receiver_buffer_arr[i], recvpkt, MSS_SIZE);
                        break;
                    }
                }
            }
        }

        // If the packet is received in order
        else if (subsequent_seqno == recvpkt->hdr.seqno)
        {
            gettimeofday(&tp, NULL);
            VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

            // Update subsequent_seqno to the next required pkt
            subsequent_seqno = recvpkt->hdr.seqno + recvpkt->hdr.data_size;

            // Check receiver buffer for additional packets
            int continue_reading;
            do
            {
                continue_reading = 0;
                for (int i = 0; i < MAX_BUFFER_SIZE; i++)
                {
                    if (receiver_buffer_arr[i]->hdr.seqno == subsequent_seqno)
                    {
                        fseek(fp, receiver_buffer_arr[i]->hdr.seqno, SEEK_SET);
                        fwrite(receiver_buffer_arr[i]->data, 1, receiver_buffer_arr[i]->hdr.data_size, fp);
                        subsequent_seqno = receiver_buffer_arr[i]->hdr.seqno + receiver_buffer_arr[i]->hdr.data_size;
                        receiver_buffer_arr[i]->hdr.seqno = -1;
                        receiver_buffer_arr[i]->hdr.data_size = 0;
                        continue_reading = 1;
                    }
                }
            } while (continue_reading);
        }

        // Send ACK for received packet
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = subsequent_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        sndpkt->hdr.time_stamp = recvpkt->hdr.time_stamp;

        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                   (struct sockaddr *)&clientaddr, clientlen) < 0)
        {
            error("ERROR in sendto");
        }
    }

    return 0;
}
