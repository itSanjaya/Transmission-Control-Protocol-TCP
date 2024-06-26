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
#include "create_window.h"

tcp_packet *recvpkt;
tcp_packet *sndpkt;

// our receive base starts at seqno 0
int recv_base = 0;

// looks for sequential packets in the window and writes them to the file
void write_to_file(FILE* fp, int seqno, window* recv_window) {
    node* curr = recv_window->head->next;

    // checks if the seqno of the current packet matches the receive base, if it does it means that the packet is in order
    while (curr != recv_window->tail && curr->pkt_seqno == recv_base) {
        fwrite(curr->data, 1, curr->data_length, fp);

        // updates the receive base such that it is the seqno of the next packet, so that we can check if the next packet is in order
        recv_base += curr->data_length;
        curr = curr->next;

        // removes the packet which have already been written to the file
        erase_node(recv_window, curr->prev);
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

    fp  = fopen(argv[2], "wb");
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

    // creating the receive window
    window *recv_window = create_window();

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

        // if the packet is empty, it means that the file has been completely received
        if ( recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");
            // sndpkt = make_packet(0);
            // sndpkt->hdr.ackno = -1;

            // sndpkt->hdr.seqno = recvpkt->hdr.seqno;
            // sndpkt->hdr.ctr_flags = ACK;
            // if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
            //         (struct sockaddr *) &clientaddr, clientlen) < 0) {
            //     error("ERROR in sendto");
            // }
            fclose(fp);
            break;
        }
        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        
        // if the packet is out of order, we send an ack with the current receive base
        // in this case, the packet received is less than the receive base which means that it is a duplicate packet
        if (recvpkt->hdr.seqno < recv_base) {
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = recv_base;
            
            // we record the sequence number of the packet that we received, so that the client knows which packet is ACKing
            sndpkt->hdr.seqno = recvpkt->hdr.seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
        }
        else{
            // checking if the packet being sent can be fitted into the receive window
            if (recvpkt->hdr.seqno >= recv_base) {

                // we buffer all the packets in the window
                recv_add_node(recv_window, recvpkt->data, recvpkt->hdr.data_size, recvpkt->hdr.seqno);

                // we write to the file all the packets that are in order
                write_to_file(fp, recvpkt->hdr.seqno, recv_window);

                // sending cumulative acks
                sndpkt = make_packet(0);
                sndpkt->hdr.ackno = recv_base;

                // we record the sequence number of the packet that we received, so that the client knows which packet is ACKing
                sndpkt->hdr.seqno = recvpkt->hdr.seqno;
                sndpkt->hdr.ctr_flags = ACK;
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                        (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }
            }
        }

        VLOG(INFO, "Window Size: %d, Recv Base: %d", recv_window->num_of_nodes, recv_base);
    }

    close(sockfd);
    free_window(recv_window);

    return 0;
}
