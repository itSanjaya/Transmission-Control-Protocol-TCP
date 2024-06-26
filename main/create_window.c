#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<sys/time.h>

#include"create_window.h"
//create a buffer window with at most size 10
window* create_window(){
    window* w = malloc(sizeof(window));

    w->head = malloc(sizeof(node));
    w->tail = malloc(sizeof(node));

    w->head->next = w->tail;
    w->head->prev = NULL;

    w->tail->next = NULL;
    w->tail->prev = w->head;

    //initializing default starting values for the window
    w->next_seqno = 0;
    w->num_of_nodes = 0;
    w->send_base = 0;

    return w;
}

//Implements insertion sorting algorithm for the rerceiver buffer
void insert_node(window*w , node* new_node){
    node* curr = w->head->next;
    while(curr != w->tail && curr->pkt_seqno < new_node->pkt_seqno){
        curr = curr->next;
    }

    if (curr != w->tail && curr->pkt_seqno == new_node->pkt_seqno){
        return;
    }

    new_node->next = curr;
    new_node->prev = curr->prev;

    curr->prev->next = new_node;
    curr->prev = new_node;
}

//Adds a node to the receiver buffer
void recv_add_node(window * w, char data[DATA_SIZE], int data_length, int pkt_seqno){
    node* new_node = malloc(sizeof(node));

    memcpy(new_node->data, data, DATA_SIZE);
    new_node->data_length = data_length;

    new_node->pkt_seqno = pkt_seqno;

    insert_node(w, new_node); //to implement insertion sorting

    w->num_of_nodes++; //increment the number of nodes in the window
}

//Adds a node to the sender buffer
void sender_add_node(window * w, char data[DATA_SIZE], int data_length){
    node* new_node = malloc(sizeof(node));

    memcpy(new_node->data, data, DATA_SIZE);
    new_node->data_length = data_length;

    new_node->pkt_seqno = w->next_seqno;
    w->next_seqno += data_length; //this changes the next packet we are expecting for the window

    struct timeval curr_time;
    gettimeofday(&curr_time, NULL);
    new_node->sent_time = curr_time;

    // initializing default values for the node
    new_node->num_resent = 0;
    new_node->acked = 0;
    new_node->num_timeout = 0;

    new_node->next = w->tail;
    new_node->prev = w->tail->prev;
    w->tail->prev->next = new_node;
    w->tail->prev = new_node;

    w->num_of_nodes++;
}

//Removes a node from the sender buffer
void erase_node(window * w, node* n){
    n->prev->next = n->next;
    n->next->prev = n->prev;

    free(n);
    w->num_of_nodes--; //decrement the number of nodes in the window
}

//Removes all the nodes from the sender buffer that have been acknowledged
void remove_node(window * w, int ackno){
    node* curr = w->head->next;
    while(curr != w->tail && curr->pkt_seqno < ackno){
        curr = curr->next;
        erase_node(w, curr->prev);
    }
}

void except_first(window * w){
    
}

//Freeing all the memory allocated for the window
void free_window(window * w){
    node* curr = w->head->next;
    while(curr != w->tail){
        curr = curr->next;
        erase_node(w, curr->prev);
    }

    free(w->head);
    free(w->tail);
    free(w);
}