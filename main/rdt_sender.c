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
#include <pthread.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>

#include"create_window.h"
#include"common.h"

#define STDIN_FD    0
#define NUM_THREADS 2

// defing the constants for rto calculation
#define ALPHA 0.125
#define BETA 0.25
#define RTO_MAX 240000

// defining the different states of the congestion control
#define SLOW_START 0
#define CONGESTION_AVOIDANCE 1
#define FAST_RETRANSMIT 2

// window size of 1
float window_size = 1.0;

// initializing the variables for rto calculation and congestion control
int rto = 3000; // 3 seconds
int rto_exp = 3000;
float sample_rtt = 0;
float estimated_rtt = 0;
float dev_rtt = 0;
int ss_thresh = 64;

// initializing the amount of exponential backoff, this doubles the RTO every time we have a timeout
int exp_backoff = 2;

// initializing the state of the congestion control
int state = SLOW_START;

window* sender_window;

// to know if the end of file has been reached
int file_size;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;   

// making the file global to access it anywhere
FILE *fp;

// creates the CWND.csv file for reviewing the congestion window
FILE *cwnd_file;

// get the difference in time to calculate the sample rtt
float timedifference_msec(struct timeval t0, struct timeval t1)
{
    return fabs((t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
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

// function to calculate the maximum of two numbers
int max(int a, int b) {
    if (a > b) {
        return a;
    }
    return b;
}

// main function that implements the congestion conrtrol mechanism
void cong_control(int dup) {
    // we increase the window size by 1 every ACK we receive
    if (state == SLOW_START) {
        window_size++;
        if (window_size >= ss_thresh) {
            state = CONGESTION_AVOIDANCE;
        }
    } 
    // we increase the window size by 1/window_size every ACK we receive after we reach the threshold
    else if (state == CONGESTION_AVOIDANCE) {
        window_size += (float) (1.0 / (int)(window_size));
    }
    
    // we enter fast retransmit if we receive 3 duplicate ACKs
    else if (state == FAST_RETRANSMIT) {
        ss_thresh = max( (int) (window_size / 2), 2);
        window_size = 1;
        state = SLOW_START;

        if (dup) {
            // we delete all the packets except for the first one
            // except_first(sender_window);

            // we reset the file pointer to the next packet to be sent
            // fseek(fp, sender_window->next_seqno, SEEK_SET);
        }
    }

    VLOG(INFO, "Window size is %f", window_size);

    // write the time, window size and threshold to the cwnd file
    struct timeval curr_time;
    gettimeofday(&curr_time, NULL);

    // writing the necessary data into the CWND.csv file
    fprintf(cwnd_file, "%ld,%f,%d\n", curr_time.tv_sec * 1000 + curr_time.tv_usec / 1000, window_size, ss_thresh);
}

// we resend the packet if we don't receive an ACK within 120ms
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Timeout happened"); // NOTE: This is printed in case of duplicate ACKs as well

        node * curr = sender_window->head->next;

        // filter to check if the window is empty
        if (curr == sender_window->tail)
        {
            VLOG(INFO, "No packets to resend");
            return;
        }

        // making the packet and sending it
        sndpkt = make_packet(curr->data_length);
        memcpy(sndpkt->data, curr->data, curr->data_length);
        sndpkt->hdr.seqno = curr->pkt_seqno;

        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }

        // resending the packet, so we increase the counter
        curr->num_resent++;

        // we also record the number of times the packet has timed out
        curr->num_timeout++;

        // since timeout indicates a packet loss, we enter fast retransmit
        state = FAST_RETRANSMIT;
        cong_control(0);

        // updating the timestamp of the packet
        gettimeofday(&curr->sent_time, NULL);

        if (curr->num_timeout >= 2) {
            // exponential backoff

            // we are preserving the rto value so that we can use it for other packets that are not experiencing exponential backoff
            if (curr->num_timeout == 2) {
                rto_exp = rto;
            }

            // we double the rto value after we experience two successive timeouts
            rto_exp *= exp_backoff;
            if (rto_exp > RTO_MAX) {rto_exp = RTO_MAX;}

            VLOG(INFO, "Exponential backoff: RTO is %d", rto_exp);
        }

        // restart the timer
        stop_timer();
        init_timer(rto_exp, resend_packets);
        start_timer();

        VLOG(INFO, "Resent packet with seqno %d", curr->pkt_seqno);
    }
}

// we resend the packet if we receive 3 duplicate ACKs
void resend_duplicate_packets(int sig)
{
    if (sig == SIGALRM)
    {
        VLOG(INFO, "Duplicate ACK Detected!"); // NOTE: This is printed in case of duplicate ACKs as well

        node * curr = sender_window->head->next;

        // filter to check if the window is empty
        if (curr == sender_window->tail)
        {
            VLOG(INFO, "No packets to resend");
            return;
        }

        // resending the packet, so we increase the counter
        curr->num_resent++;

        // 3 duplicate ACKs indicate a packet loss, so we enter fast retransmit
        state = FAST_RETRANSMIT;
        cong_control(1);

        // updating the timestamp of the packet
        gettimeofday(&curr->sent_time, NULL);

        // making the packet and sending it
        sndpkt = make_packet(curr->data_length);
        memcpy(sndpkt->data, curr->data, curr->data_length);
        sndpkt->hdr.seqno = curr->pkt_seqno;

        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }

        VLOG(INFO, "Resent packet with seqno %d", curr->pkt_seqno);
    }
}

// send the packet in the last node of the window
void send_packet(int sockfd){
    // the last node is the latest addition to the window
    node* to_send = sender_window->tail->prev;
    int data_size = to_send->data_length;

    // making the packet and sending it
    sndpkt = make_packet(data_size);
    memcpy(sndpkt->data, to_send->data, data_size);
    sndpkt->hdr.seqno = to_send->pkt_seqno;

    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
    {
        error("sendto");
    }

    VLOG(INFO, "Sent packet with seqno %d", to_send->pkt_seqno);
}

// calculates the RTO value
void calculate_rto(int seqno) {
    // get the time when the packet with the given seqno was sent
    node * curr = sender_window->head->next;
    while (curr != sender_window->tail && curr->pkt_seqno != seqno) {
        curr = curr->next;
    }

    // if the packet was not found, we don't do anything
    if (curr == sender_window->tail) {
        return;
    }

    // get the timestamp of the packet
    struct timeval sent_time = curr->sent_time;

    // get the current time
    struct timeval curr_time;
    gettimeofday(&curr_time, NULL);

    // calculate the sample RTT
    sample_rtt = timedifference_msec(sent_time, curr_time);

    // calculate the estimated RTT and the deviation RTT
    estimated_rtt = (1 - ALPHA) * estimated_rtt + ALPHA * sample_rtt;
    dev_rtt = (1 - BETA) * dev_rtt + BETA * fabs(sample_rtt - estimated_rtt);

    // calculate the RTO
    rto = (int) estimated_rtt + 4 * dev_rtt;
    if (rto > RTO_MAX) {rto = RTO_MAX;}
    // if (rto < RTO_MIN) {rto = RTO_MIN;}

    VLOG(INFO, "Calculated RTO is %d", rto);

    // we reset the timer
    stop_timer();
    init_timer(rto, resend_packets);
    start_timer();

}

// this runs in a separate thread to receive the ACKs and update the window in parallel
void* receive_ack(void * arg){
    // we detach the thread because no other thread needs to join with it
    pthread_detach(pthread_self());
    int sockfd = (intptr_t)arg;

    // we keep track of the number of duplicate ACKs received
    int duplicate_ack = 0;
    
    char buffer[MSS_SIZE];
    while(1){
        // receive the ACK
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0, 
            (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }
        recvpkt = (tcp_packet *)buffer;

        VLOG(INFO, "Received ACK for packet with seqno %d from packet %d", recvpkt->hdr.ackno, recvpkt->hdr.seqno);

        assert(get_data_size(recvpkt) <= DATA_SIZE);

        // if we receive an ACK it means that a packet was received successfully
        cong_control(0);

        // we received the oldest unACKed packet, so we stop the timer and update the send_base
        if (recvpkt->hdr.ackno > sender_window->send_base){
            sender_window->send_base = recvpkt->hdr.ackno;
            stop_timer();

            // we calculate the RTO of packets which are never resent
            if (sender_window->head->next->num_resent == 0) {
                calculate_rto(sender_window->head->next->pkt_seqno);
            }
            
            // fseek(fp, recvpkt->hdr.ackno, SEEK_SET);
            
            // we remove all the packets that have been cumulatively ACKed
            remove_node(sender_window, recvpkt->hdr.ackno);
            bzero(buffer, MSS_SIZE);            
        }

        // if we receive a duplicate ACK, we increment the duplicate ACK counter
        if (recvpkt->hdr.ackno < recvpkt->hdr.seqno){
            duplicate_ack++;
            calculate_rto(recvpkt->hdr.seqno);
        }

        // if we receive 3 duplicate ACKs, we resend the packet and restart the timer
        if (duplicate_ack == 3){
            VLOG(INFO, "Duplicate ACK received");
            duplicate_ack = 0;
            stop_timer();
            resend_duplicate_packets(SIGALRM);
            start_timer();
        }

        VLOG(INFO, "Num2: %d", sender_window->num_of_nodes);

        // if we have sent all the packets, we break out of the loop, and waiting for the last packet to be ACKed
        if (sender_window->send_base == file_size){
            break;
        }
    }

    pthread_exit(NULL);
}

int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "rb");
    if (fp == NULL) {
        error(argv[3]);
    }

    // get file size
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // making the cwnd file
    cwnd_file = fopen("../obj/CWND.csv", "w");
    if (cwnd_file == NULL) {
        error("CWND.csv");
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

    init_timer(rto, resend_packets);

    // create the sender window
    sender_window = create_window();

    // create a thread to receive the ACKs
    pthread_t threads[NUM_THREADS];
    int rc = pthread_create(&threads[0], NULL, receive_ack, (void *)(intptr_t)sockfd);
    if (rc){
        printf("ERROR: Unable to create thread %d\n", rc);
        exit(-1);
    }

    while (1)
    {
        // limit the number of packets in the window to the window size
        if (sender_window->num_of_nodes <= (int) window_size){
            VLOG(INFO, "Number of Nodes: %d", sender_window->num_of_nodes);

            // read the data from the file
            len = fread(buffer, 1, DATA_SIZE, fp);

            if (len > 0){
                // create a packet and add it to the window
                sender_add_node(sender_window, buffer, len);
                bzero(buffer, DATA_SIZE);

                // send the packet
                send_packet(sockfd);

                // start the timer if this is the first packet in the window, the other start_timer() calls are in the receive_ack() function
                if (sender_window->send_base == 0){
                    start_timer();
                }
            }
            else{
                // if we have read all the data from the file, we break out of the loop
                break;
            }
            VLOG(INFO, "Send Base: %ld", sender_window->send_base);
        }
    }

    // wait for all the packets to be ACKed
    while(sender_window->send_base != sender_window->next_seqno){
        VLOG(INFO, "Waiting for ACKs");
        sleep(1);
    }

    VLOG(INFO, "Buffer Size: %d", sender_window->num_of_nodes);

    // send the final packet to mark the end of file
    VLOG(INFO, "End of File Reached");


    // we are making sure that the last packet is sent. So, we send it multiple times.
    int count = 0;
    do {
        sndpkt = make_packet(0);
        sndpkt->hdr.seqno = sender_window->next_seqno;
        sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
            ( const struct sockaddr *)&serveraddr, serverlen);

        count++;

        if (count == 10){
            break;
        }

        VLOG(INFO, "Sending FIN");
    }while(1);

    close(sockfd);
    free_window(sender_window);
    free(sndpkt);
    
    pthread_exit(NULL);
}



