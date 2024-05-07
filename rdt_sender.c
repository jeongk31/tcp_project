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
#include <math.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD 0
#define MAX_WINDOW_SIZE 100000
int retry_in_ms = 50;
int slow_start_threshold = 64;
int dup_ack_seqno = 0;
int whole_number = 0;
int next_seqno = 0;
int send_base = 0;
int window_size = 1;
float estimated_RTT = 10;
float sample_RTT = 50;
float deviation_RTT = 0;

struct timeval start_time, finish_time; // For putting time_stamps into packets for recalculating RTT
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;
tcp_packet *send_window[MAX_WINDOW_SIZE];

void resend_packet(int *next_seqno)
{
    // Calculate the index of the packet to be resent
    int index_to_send = send_base / DATA_SIZE;

    // Add timestamp to the packet being resent
    gettimeofday(&start_time, NULL);
    send_window[index_to_send]->hdr.time_stamp = (start_time.tv_sec * 1000LL + (start_time.tv_usec / 1000));

    // Send the packet
    if (sendto(sockfd, send_window[index_to_send], TCP_HDR_SIZE + get_data_size(send_window[index_to_send]), 0,
               (const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }

    // Update congestion control parameters
    slow_start_threshold = (int)ceil((double)window_size / (double)2);
    printf("After retransmission, SSTHRESH is %d\n", slow_start_threshold);
    window_size = 1;
    whole_number = 0;

    // Update the next sequence number
    *next_seqno = send_window[index_to_send]->hdr.seqno + send_window[index_to_send]->hdr.data_size;
}

void signal_handler(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timout happend");
        resend_packet(&next_seqno);
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

// Function to update the timeout period (DELAY based on the RTT)
void update_timer(tcp_packet *packet)
{
    gettimeofday(&finish_time, NULL);
    sample_RTT = (float)((finish_time.tv_sec * 1000LL + (finish_time.tv_usec / 1000)) - packet->hdr.time_stamp); // In milli sec

    // Calculate the extiamted RTT and Deviation in RTT
    estimated_RTT = ((1.0 - 0.125) * estimated_RTT) + (0.125 * sample_RTT);
    deviation_RTT = ((1.0 - 0.25) * deviation_RTT) + (0.25 * fabs(sample_RTT - estimated_RTT));

    // Calculating the delay or the retry time
    retry_in_ms = (int)(estimated_RTT + (4 * deviation_RTT));
}

/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 */
void init_timer(int delay)
{
    signal(SIGALRM, signal_handler);
    timer.it_interval.tv_sec = delay / 1000; // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000; // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

int main(int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    char recv_buffer[DATA_SIZE];
    int next_packet_index = 0;
    int current_index = 0;
    int current_window_size = 0;
    int to_be_sent = 0;
    FILE *fp;
    FILE *csv_fp;

    csv_fp = fopen("CWND.csv", "w");
    /* check command line arguments */
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL)
    {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0)
    {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    int insert_position_buffer = 0;
    int base = 0;
    while (1)
    {
        len = fread(buffer, 1, DATA_SIZE, fp);
        if (len <= 0)
        {
            sndpkt = make_packet(MSS_SIZE);
            sndpkt->hdr.data_size = 0;
            sndpkt->hdr.seqno = -1;
            send_window[insert_position_buffer] = (tcp_packet *)sndpkt;
            break;
        }

        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = base;
        base += len;

        send_window[insert_position_buffer] = (tcp_packet *)sndpkt;
        insert_position_buffer++;
    }

    // Stop and wait protocol
    init_timer(retry_in_ms);
    next_seqno = 0;
    send_base = 0;
    insert_position_buffer = 0;

    for (int i = 0; i < window_size; i++)
    {
        int length = send_window[insert_position_buffer]->hdr.data_size;
        int send_base_initial = next_seqno;
        next_seqno = send_base_initial + length;

        // Adding a begin timeval to the packet being sent
        gettimeofday(&start_time, NULL);
        send_window[insert_position_buffer]->hdr.time_stamp = (start_time.tv_sec * 1000LL + (start_time.tv_usec / 1000));

        VLOG(DEBUG, "Sending packet %d to %s",
             send_base_initial, inet_ntoa(serveraddr.sin_addr));
        /*
         * If the sendto is called for the first time, the system will
         * assign a random port number so that server can send its
         * response to the src port.
         */
        if (sendto(sockfd, send_window[insert_position_buffer], TCP_HDR_SIZE + get_data_size(send_window[insert_position_buffer]), 0,
                   (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }

        insert_position_buffer++;
    }

    // main loop
    
    int end_of_file_reached = 0;
    int duplicate_ack_count = 0;

    // Waiting for ACK
    do
    {
        start_timer();
        if (recvfrom(sockfd, recv_buffer, MSS_SIZE, 0,
                     (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }
        recvpkt = (tcp_packet *)recv_buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        // Calculating the new value of the retry_in_ms timeout time
        update_timer(recvpkt);

        // Test for triple duplicate acks
        if (dup_ack_seqno == recvpkt->hdr.ackno)
        {
            duplicate_ack_count += 1;
        }
        dup_ack_seqno = recvpkt->hdr.ackno;

        // If fourth duplicate acknoledgements have been received
        if (duplicate_ack_count >= 4)
        {
            duplicate_ack_count = 0;
            resend_packet(&next_seqno);
            printf("4 duplicate acks forced timeout\n");
            continue;
        }

        if (send_base <= dup_ack_seqno)
        {
            while (send_base < dup_ack_seqno)
            {
                next_packet_index = (int)ceil((double)next_seqno / (double)DATA_SIZE);

                if (send_window[next_packet_index]->hdr.seqno == -1 && send_window[next_packet_index]->hdr.data_size == 0)
                {
                    end_of_file_reached = 1;
                }

                if (end_of_file_reached == 1 && dup_ack_seqno == next_seqno)
                {
                    VLOG(INFO, "End of File reached. Sending final packet to ensure receiver acknowledgment.");
                    sndpkt = make_packet(0);
                    // Final zero_packet is sent multiple times to ensure that receiver gets this packet
                    int resend_final_pkt = 0;
                    while (resend_final_pkt < 5)
                    {
                        sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                               (const struct sockaddr *)&serveraddr, serverlen);
                        resend_final_pkt++;
                    }
                    fclose(csv_fp);
                    return 0;
                }

                current_index = send_base / DATA_SIZE;
                send_base = send_base + send_window[current_index]->hdr.data_size;

                VLOG(DEBUG, "Packet %d sent to %s.%d Next expected ACK: %d",
                     send_base, inet_ntoa(serveraddr.sin_addr), portno, dup_ack_seqno);

                // if end of file has been reached set send_base to dup_ack_seqno
                if (end_of_file_reached == 1)
                {
                    send_base = dup_ack_seqno;
                }
                else
                {
                    printf("Threshold: %d\n", slow_start_threshold);
                    printf("Window size: %d\n", window_size);
                    gettimeofday(&start_time, NULL);
                    send_window[next_packet_index]->hdr.time_stamp = (start_time.tv_sec * 1000LL + (start_time.tv_usec / 1000));

                    if (sendto(sockfd, send_window[next_packet_index], TCP_HDR_SIZE + get_data_size(send_window[next_packet_index]), 0,
                               (const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto");
                    }
                    next_seqno += send_window[next_packet_index]->hdr.data_size;

                    gettimeofday(&start_time, NULL);

                    // congestion window size values and time values to the csv file
                    fprintf(csv_fp, "%llu,%i,%d\n",(start_time.tv_sec * 1000LL + (start_time.tv_usec / 1000)), window_size, slow_start_threshold);
                    if (window_size <= slow_start_threshold)
                    {
                        printf("SLOW START\n");
                        window_size++;
                    }
                    else
                    {
                        whole_number++;
                        printf("Congestion Avoidance logic\n");
                        printf("Whole number: %d\n", whole_number);

                        if (whole_number > window_size)
                        {
                            window_size++;
                            whole_number = 0;
                            printf("New Window size: %d\n", window_size);
                        }
                    }
                }
            }

            current_window_size = (int)ceil((double)(next_seqno - send_base) / (double)DATA_SIZE);
            to_be_sent = window_size - current_window_size;

            while (to_be_sent > 0)
            {
                next_packet_index = (int)ceil((double)next_seqno / (double)DATA_SIZE);

                if (send_window[next_packet_index]->hdr.seqno == -1 && send_window[next_packet_index]->hdr.data_size == 0)
                {
                    break;
                }

                gettimeofday(&start_time, NULL);
                send_window[next_packet_index]->hdr.time_stamp = (start_time.tv_sec * 1000LL + (start_time.tv_usec / 1000));

                if (sendto(sockfd, send_window[next_packet_index], TCP_HDR_SIZE + get_data_size(send_window[next_packet_index]), 0,
                           (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }
                next_seqno += send_window[next_packet_index]->hdr.data_size;
                to_be_sent--;
            }
            current_window_size = (int)ceil((double)(next_seqno - send_base) / (double)DATA_SIZE);
        }
        stop_timer();
    } while (1);
    return 0;
}