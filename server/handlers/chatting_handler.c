#include "chatting_handler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "server_state.h"
#include "user_collection.h"
#include "user.h"
#include "server_responses.h"
#include "pc_collection.h"
#include "gc_collection.h"
#include "chat_entry.h"
#include "packet_format.h"

//TODO: refactor this file, there are a lot of functions that shouldn't be here

//should probably move it to chat_entry later
struct chat_entry *build_chat_entry_from_packet(struct packet *pack){
    struct chat_entry *cent = calloc(1, sizeof(struct chat_entry));
    chat_entry_init(cent);
    
    cent->chatter_id = pack->header.sender_id;
    cent->type = pack->header.op_code;
    
    cent->load_length = pack->header.payload_length;
    if(pack->header.payload_length != 0){
        cent->load = calloc(1, cent->load_length);
    
        strncpy(cent->load, pack->payload, cent->load_length);
    }

    return cent;
}


//this should go to server_responses
//If I'll need to keep track of unsent messages, this could be a good point to do it
void send_message_to_user(struct server_context *server,struct packet *pack, uint32_t global_user_id){
    struct user *usr = ucol_find_user_by_id(&users, global_user_id);

    if(usr != NULL && usr->is_logged_in){
        server_send_message(server, usr->logged_in_from, pack);
    }
}


#define NUMBER_OF_USERS_IN_PRIVATE_CHAT 2
void relay_message_in_private_chat(struct server_context *server, struct private_chat *pc, struct packet *msg, uint32_t ignore){
    uint32_t users[NUMBER_OF_USERS_IN_PRIVATE_CHAT];
    pc_get_recipients(pc, users, users + 1);

    for(uint32_t i = 0; i < NUMBER_OF_USERS_IN_PRIVATE_CHAT; i++){
        send_message_to_user(server, msg, users[i]);
    }
}


// I don't think this should be moved anywhere, or I can create some new header
uint32_t privmsg_in_existing_private_chat(struct event *event, struct private_chat *pc){
    struct chat_entry *cent = build_chat_entry_from_packet(event->packet);
    pc_add_message(pc, cent);
    uint32_t message_id = event->packet->header.message_id = cent->message_id;

    relay_message_in_private_chat(event->server, pc, event->packet, event->packet->header.sender_id);

    free(cent);

    return message_id;
}


void private_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
    }

    struct user *usr = event->client_persistent_data;
    uint32_t sender = event->packet->header.sender_id;
    uint32_t receiver = event->packet->header.receiver_id;
    
    perror("I've received a personal package");
    struct private_chat *pc = pccol_find_chat_by_two_users(&pcs, sender, receiver);
    if(pc != NULL){
        perror("Found the chat, trying to send the message");
        //if such a chat exists
        if(!pc_is_pc_blocked(pc)){
            uint32_t mid = privmsg_in_existing_private_chat(event, pc);
            perror("Successfully sent");
            //response_ACK_set_message_id(event, mid);
        } else{
            response_NACK(event);
        }

    } else if(ucol_find_user_by_id(&users, receiver) != NULL){
        //if such a chat doesn't exist and the user exists
        pc = create_new_private_chat_between_users(&pcs, sender, receiver);
        privmsg_in_existing_private_chat(event, pc);
        //response_ACK(event);
    } else{
        //if chat doesn't exist and the user doesn't exist deny
        response_NACK(event);
    }
}


void broadcast_to_group_chat(struct server_context *server,struct packet *pack, struct group_chat *gc, uint32_t sender){
    struct id_collection *members = gc_get_list_of_listeners(gc);
    
    if(members != NULL){
        for(uint32_t i = 0; i < members->number_of_ids; i++){
            send_message_to_user(server, pack, members->ids[i]);
        }
    }
}


uint32_t send_message_in_existing_group_chat(struct event *event, struct group_chat *gc){
    struct chat_entry *cent = build_chat_entry_from_packet(event->packet);
    gc_add_message(gc, cent);
    uint32_t message_id = event->packet->header.message_id = cent->message_id;

    broadcast_to_group_chat(event->server, event->packet, gc, event->packet->header.sender_id);

    free(cent);

    return message_id;
}


void group_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct group_chat *gc = gccol_find_chat_by_group_id(&gcs, event->packet->header.receiver_id);

    if(gc != NULL && gc_is_user_listening_to_gc(gc, event->packet->header.sender_id)){
        uint32_t mid = send_message_in_existing_group_chat(event, gc);
        //response_ACK_set_message_id(event, mid);
    } else{
        response_NACK(event);
    }
}


bool delete_message_in_existing_group_chat(struct event *event, struct group_chat *gc){
    uint32_t message_id = event->packet->header.target;

    gc_delete_message(gc, message_id);
    broadcast_to_group_chat(event->server, event->packet, gc, event->packet->header.sender_id);

    return true;
}


void delete_group_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct group_chat *gc = gccol_find_chat_by_group_id(&gcs, event->packet->header.receiver_id);

    if(gc != NULL){
        bool success = delete_message_in_existing_group_chat(event, gc);
        if(success)
            response_ACK(event);
        else
            response_NACK(event);
    } else{
        response_NACK(event);
    }
}


bool delete_message_in_existing_private_chat(struct event *event, struct private_chat *pc){
    uint32_t message_id = event->packet->header.target;

    pc_delete_message(pc, message_id);

    relay_message_in_private_chat(event->server, pc, event->packet, event->packet->header.sender_id);

    return true;
}


void delete_private_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
    }

    uint32_t sender = event->packet->header.sender_id;
    uint32_t receiver = event->packet->header.receiver_id;

    struct private_chat *pc = pccol_find_chat_by_two_users(&pcs, sender, receiver);
    if(pc != NULL){
        bool success = delete_message_in_existing_private_chat(event, pc);
        if(success)
            response_ACK(event);
        else
            response_NACK(event);
    } else{
        response_NACK(event);
    }
}

//add the similar function to deletion later, I forgot about them
bool authorised_to_edit(struct user *usr, struct chat_entry *cent){
    return cent->chatter_id == usr->id;
}

//also move it somewhere
struct packet *produce_packet_from_chat_entry(struct chat_entry *entry, uint32_t receiver){
    struct packet *pack = calloc(1, sizeof(struct packet));
    packet_init(pack);
    pack->header.message_id = entry->message_id;
    pack->header.op_code = entry->type;
    pack->header.payload_length= entry->load_length;
    pack->header.receiver_id = receiver;
    pack->header.sender_id = entry->chatter_id;
    pack->payload = entry->load;
    
    return pack;
}


void edit_group_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    uint32_t sender = event->packet->header.sender_id;
    struct group_chat *gc = gccol_find_chat_by_group_id(&gcs, event->packet->header.receiver_id);

    if(gc == NULL){
        response_NACK(event);
        return;
    }

    struct chat_entry *cent = gc_find_chat_entry_by_message_id(gc, event->packet->header.target);
    if(cent == NULL){
        response_NACK(event);
        return;
    }
    if(!authorised_to_edit(event->client_persistent_data, cent)){
        response_NACK(event);
        return;
    }

    free(cent->load);
    cent->load_length = event->packet->header.payload_length;
    cent->load = calloc(1, cent->load_length);
    strncpy(cent->load, event->packet->payload, cent->load_length);

    struct packet *pack = produce_packet_from_chat_entry(cent, event->packet->header.receiver_id);
    broadcast_to_group_chat(event->server, pack, gc, event->packet->header.sender_id);

    response_ACK(event);
}


void edit_private_message(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct private_chat *pc = pccol_find_chat_by_two_users(&pcs, event->packet->header.sender_id, event->packet->header.receiver_id);
    if(pc == NULL){
        response_NACK(event);
        return;
    }

    struct chat_entry *cent = pc_find_chat_entry_by_message_id(pc, event->packet->header.target);
    if(cent == NULL){
        response_NACK(event);
        return;
    }
    if(!authorised_to_edit(event->client_persistent_data, cent)){
        response_NACK(event);
        return;
    }

    free(cent->load);
    cent->load_length = event->packet->header.payload_length;
    cent->load = calloc(1, cent->load_length);
    strncpy(cent->load, event->packet->payload, cent->load_length);

    struct packet *pack = produce_packet_from_chat_entry(cent, event->packet->header.receiver_id);
    relay_message_in_private_chat(event->server, pc, pack, event->packet->header.sender_id);
    
    response_ACK(event);
}

void create_group(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct group_chat gc;
    group_chat_init(&gc);

    gccol_add_gchat(&gcs, &gc);
    response_ACK_set_message_id(event, gc.chat_id);
    printf("NOTICE: A new chat was created. It's id is %u\n", gc.chat_id);
}

void join_group(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct group_chat *gc = gccol_find_chat_by_group_id(&gcs, event->packet->header.target);

    if(gc != NULL && !gc_is_user_listening_to_gc(gc, event->packet->header.sender_id)){
        gc_add_listener(gc, event->packet->header.sender_id);
        response_ACK(event);
    }else{
        response_NACK(event);
    }
}

void leave_group(struct event *event){
    if(event->client_persistent_data == NULL){
        response_NACK(event);
        return;
    }

    struct group_chat *gc = gccol_find_chat_by_group_id(&gcs, event->packet->header.target);

    if(gc != NULL){
        gc_remove_listener(gc, event->packet->header.sender_id);
        response_ACK(event);
    }else{
        response_NACK(event);
    }
}