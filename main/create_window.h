#include<stdio.h>
#include<stdlib.h>
#include<sys/time.h>
#include"packet.h"

//implementation of a linked list for the buffer window
typedef struct node {
    struct node * next;
    struct node * prev;
    int pkt_seqno; //the sequence number of the packet
    int data_length; //the length of the data in the packet
    char data[DATA_SIZE]; //the data in the packet
    struct timeval sent_time; //the time the packet was sent
    int num_resent; //the number of times the packet has been resent
    int acked; //whether the packet has been acked or not
    int num_timeout; //the number of times the packet has timed out
} node;

typedef struct {
    node * head;
    node * tail;
    int num_of_nodes; //the number of nodes (a.k.a. packets) in the window
    long next_seqno; //the sequence number of the next packet the window expects to get
    long send_base; //the sequence number of the oldest unacked packet in the window
} window;

window * create_window();
void recv_add_node(window * w, char data[DATA_SIZE], int data_length, int pkt_seqno);
void sender_add_node(window * w, char data[DATA_SIZE], int data_length);
void erase_node(window * w, node* n);
void remove_node(window * w, int ackno);
void except_first(window * w);
void free_window(window * w);