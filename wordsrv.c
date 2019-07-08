#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 58231
#endif
#define MAX_QUEUE 5


void add_player(struct client **top, int fd, struct in_addr addr, int print_indicator);
void remove_player(struct client **top, int fd);
void disconnect(struct client*p, struct game_state* game, int* restore_indicator);
void announce_turn(struct game_state *game, int* restore_indicator);

/* These are some of the function prototypes that we used in our solution 
 * You are not required to write functions that match these prototypes, but
 * you may find the helpful when thinking about operations in your program.
 */
/* Send the message in outbuf to all clients */
void broadcast(struct game_state *game, char *outbuf, int* restore_indicator){
    struct client* cpy = game->head;
    while (cpy != NULL){
        if(write(cpy->fd, outbuf, MAX_BUF) == -1){
            disconnect(cpy, game, restore_indicator);
        }
        cpy = cpy->next;
    }
}

void player_join_message(struct game_state*game, char* outbuf, int* restore_indicator){
    struct  client* cpy = game->head;
    while (cpy != NULL){
        if (write(cpy->fd, outbuf, MAX_BUF) == -1) {
            disconnect(cpy, game, restore_indicator);
            return;
        }
        cpy = cpy->next;
    }
}
void announce_turn(struct game_state *game, int* restore_indicator){
    if (game->head == NULL){
        return;
    }
    char msg[MAX_BUF] = "It's ";
    char your_guess[13] = "Your guess?\r\n";
    strcat(msg, game->has_next_turn->name);
    strcat(msg, "'s turn.\r\n");
    struct client * cpy = game->head;
    while (cpy != NULL){
        if (cpy->fd == game->has_next_turn->fd){
            if (write(cpy->fd, your_guess, sizeof(your_guess)) == -1){
                disconnect(cpy, game, restore_indicator);
                return;
            }
        }else {
            if (write(cpy->fd, msg, strlen(msg)) == -1) {
                disconnect(cpy, game, restore_indicator);
                return;
            }
        }
        cpy = cpy->next;
    }
}
void announce_winner(struct game_state *game, struct client *winner, int* restore_indicator){
    char self[MAX_BUF] = "You won! The word is: ";
    char other[MAX_BUF] = {'\0'};
    strcat(self, game->word);
    strcat(self, "\r\n");
    strcat(other, winner->name);
    strcat(other, " won! The word is: ");
    strcat(other, game->word);
    strcat(other, "\r\n");
    printf("%s", other);
    struct client* cpy = game->head;
    while (cpy != NULL){
        if (cpy->fd == winner->fd){
            if (write(winner->fd, self, sizeof(self))==-1){
                disconnect(winner, game, restore_indicator);
                return;
            }
        }else{
            if (write(cpy->fd, other, sizeof(other))==-1){
                disconnect(cpy, game, restore_indicator);
                return;
            }

        }
        cpy = cpy->next;
    }
}

/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game){
    if(game->head == NULL){
        game->has_next_turn = NULL;
    }else if(game->has_next_turn->next == NULL){
        game->has_next_turn = game->head;
    }else{
        game->has_next_turn = game->has_next_turn->next;
    }
}

/** Check whether the player indicated by fd is active
 *
 */
int is_active(struct client *head, int fd){
    struct client *cpy = head;
    while (cpy != NULL){
        if (cpy->fd == fd){
            return 1;
        }
        cpy = cpy->next;
    }
    return 0;
}

/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;


/* Add a client to the head of the linked list
 */
void add_player(struct client **top, int fd, struct in_addr addr, int print_indicator) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    if (print_indicator) {
        printf("Adding client %s\n", inet_ntoa(addr));
    }

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    *top = p;
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
}

int find_network_newline(const char *buf, int n) {
    for (int i = 0; i < n - 1; i++){
        if (buf[i] == '\r'){
            if (buf[i+1] == '\n'){
                return 1 + i + 1;
            }
        }
    }
    return -1;
}

int is_valid_name(struct client* head, char* name){
    if (strlen(name) >= MAX_NAME || strlen(name) == 0 || (strchr(name, ' ') != NULL && strlen(name) == 1)){
        return 0;
    }
    struct client* cpy = head;
    while (cpy != NULL){
        if(strcmp(cpy->name, name) == 0){
            return 0;
        }
        cpy = cpy->next;
    }
    return 1;

}

char inspect_input(int cur_fd, struct game_state* game, struct client* p, int*restore_indicator){

    char not_your_turn[] = "Not your turn.\r\n";
    char invalid[] = "Invalid Input.Try again\r\n";
    unsigned long prev_size = strlen(p->inbuf);
    int room = sizeof(p->inbuf)-strlen(p->inbuf);
    int nbytes;

    if ((nbytes = read(cur_fd, p->in_ptr, room)) > 0) {
        printf("Read %d bytes.\n", nbytes);
        if ((find_network_newline(p->inbuf, MAX_BUF)) > 0) {

            if (cur_fd == game->has_next_turn->fd && (strlen(p->inbuf) > 3 || (p->inbuf[0] < 97 || p->inbuf[0] > 122) || game->letters_guessed[p->inbuf[0] - 97] == 1)){
                p->in_ptr = p->inbuf;
                memset(p->inbuf, '\0', MAX_BUF);
                if (write(cur_fd, invalid, strlen(invalid)) == -1){
                    disconnect(p, game, restore_indicator);
                }
                return ' ';
            }else if (cur_fd != game->has_next_turn->fd){
                p->in_ptr = p->inbuf;
                memset(p->inbuf, '\0', MAX_BUF);
                if (write(cur_fd, not_your_turn, strlen(not_your_turn)) == -1){

                    disconnect(p, game, restore_indicator);
                    return ' ';
                }
                printf("Player %s tries to guess out of turn.\n", p->name);
                return '?';
            }
            char to_be_returned = p->inbuf[0];
            p->in_ptr = p->inbuf;
            memset(p->inbuf, '\0', MAX_BUF);
            return to_be_returned;

        }else {
            p->in_ptr = &(p->inbuf[nbytes+prev_size]);
            p->inbuf[nbytes+prev_size] = '\0';
            return '#';
        }

    }
    if (nbytes == 0){
        return '.';
    }else if(nbytes == -1){
        perror("read");
        exit(1);
    }
    return ' ';
}

/*
 * Make sure the string is read as a whole.
 */
void repeatedtively_receive_string(struct client* namer) {
    memset(namer->name, '\0', MAX_NAME);
    int room = sizeof(namer->inbuf) - strlen(namer->inbuf);
    int nbytes;
    unsigned long prev_size = strlen(namer->inbuf);
    if ((nbytes = read(namer->fd, namer->in_ptr, room)) == -1) {
        perror("read");
        exit(1);
    }
    int where;
    printf("Read %d bytes.\n", nbytes);
    if ((where = find_network_newline(namer->inbuf, MAX_BUF)) > 0) {
        namer->inbuf[where] = '\0';
        strcpy(namer->name, namer->inbuf);
        memset(namer->inbuf, '\0', MAX_BUF);
        namer->in_ptr = namer->inbuf;
        memset(&(namer->name[where-2]), '\0', MAX_NAME - where +2);
        printf("Found newline %s.\n", namer->name);
        return;

    }
    namer->in_ptr = &(namer->inbuf[prev_size + nbytes]);
    namer->inbuf[nbytes + prev_size] = '\0';
}

/*
 * Handle when a player disconnects
 */
void disconnect(struct client*p, struct game_state* game, int* restore_indicator){
    char quit_message[MAX_BUF] = {'\0'};
    strcat(quit_message, p->name);
    strcat(quit_message, " has quitted.\r\n");
    remove_player(&(game->head), p->fd);
    if (game->head == NULL){
        *restore_indicator = 0;
    }
    broadcast(game, quit_message, restore_indicator);
    printf("%s\n", quit_message);
    if (game->has_next_turn == p) {
        advance_turn(game);
    }
    announce_turn(game, restore_indicator);
    if (game->head != NULL) {
        printf("It's %s's turn.\n", game->has_next_turn->name);
    }
}

/*
 * Handle when input a valid character.
 */
void input_a_correct_character(struct game_state* game, char result, char* msg, struct client* p, int* restore_indicator){
    game->letters_guessed[result - 97] = 1;
    for (int i = 0; i < strlen(game->word); i++){
        if (game->word[i] == result){
            game->guess[i] = result;
        }
    }
    memset(msg, '\0', MAX_BUF);
    strcat(msg, p->name);
    strcat(msg, " guesses: ");
    msg[strlen(p->name) + 10] = result;
    strcat(msg, "\r\n");
    broadcast(game, msg, restore_indicator);
    memset(msg, '\0', MAX_BUF);
    status_message(msg, game);
}

/*
 * Handle the case when an incorrect character is input
 */
void input_a_incorrect_character(struct game_state* game, char result, struct client* p, char*msg, int* restore_indicator){
    game->guesses_left--;
    game->letters_guessed[result - 97] = 1;
    char wrong_msg[MAX_BUF] = "  not in the word.\r\n";
    wrong_msg[0] = result;
    printf("%s", wrong_msg);
    if (write(p->fd, wrong_msg, sizeof(wrong_msg))==-1){
        disconnect(p, game, restore_indicator);
        return;
    }
    memset(msg, '\0', MAX_BUF);
    strcat(msg, p->name);
    strcat(msg, " guesses: ");
    msg[strlen(p->name) + 10] = result;
    strcat(msg, "\r\n");
    broadcast(game, msg, restore_indicator);
    memset(msg, '\0', MAX_BUF);
    status_message(msg, game);
}

/*
 * Handle the case when a player joins after entering a valid name
 */
void join_after_enter_a_valid_name(struct client* p, struct client**new_players, struct game_state* game, int* first, char* msg, struct sockaddr_in* q, int* restore_indicator){
    char message[MAX_BUF] = {'\0'};
    char message2[] = " has joined.\r\n";
    strcpy(message, p->name);
    strcat(message, message2);
    add_player(&(game->head), p->fd, q->sin_addr, 0);
    strcpy(game->head->name, p->name);
    if(*new_players != NULL){
        *new_players = (*new_players)->next;
    }
    if (*first == 0){
        game->has_next_turn = game->head;
        (*first)++;
    }
    player_join_message(game, message, restore_indicator);
    printf("%s", message);
    memset(msg, '\0', MAX_BUF);
}




int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct sockaddr_in q;
    fd_set rset;
    char msg[MAX_BUF];
    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if(sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }
    
    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;
    int first = 0;
    int print_state = 1;
    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr, 1);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd);
            };
        }

        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                // Check if this socket descriptor is an active player
                if (is_active(game.head, cur_fd)) {
                    for (p = game.head; p != NULL; p = p->next) {
                        if (cur_fd == p->fd) {
                            //TODO - handle input from an active client
                            char result = inspect_input(cur_fd, &game, p, &first);
                            if (result == '#'){
                                print_state = 0;
                                continue;
                            }
                            print_state = 1;
                            if (result == '.'){
                                disconnect(p, &game, &first);
                                print_state = 0;
                            }else if (result != '?' && result != ' '){
                                print_state = 1;
                                printf("Found newline %c.\n", result);
                                if (strchr(game.word, result) != NULL){
                                    input_a_correct_character(&game, result, msg, p, &first);
                                    if (strchr(game.guess, '-') != NULL) {
                                        printf("It's %s's turn.\n", game.has_next_turn->name);
                                    }else{
                                        announce_winner(&game, p, &first);
                                        char start[MAX_BUF] = "Start a new game.\r\n";
                                        printf("Start a new game.\n");
                                        broadcast(&game, start, &first);
                                        init_game(&game, argv[1]);
                                        memset(msg, '\0', MAX_BUF);
                                        break;
                                    }
                                }else{
                                    print_state = 1;
                                    input_a_incorrect_character(&game, result, p, msg, &first);
                                    advance_turn(&game);
                                    if (game.guesses_left != 0) {
                                        printf("It's %s's turn.\n", game.has_next_turn->name);
                                    }else{
                                        char lose[MAX_BUF] = "The word was ";
                                        strcat(lose, game.word);
                                        strcat(lose, ". Game over.");
                                        printf("%s\n", lose);
                                        strcat(lose, "Start a new game.\r\n");
                                        printf("Start a new game.\n");
                                        broadcast(&game, lose, &first);
                                        init_game(&game, argv[1]);
                                        memset(msg, '\0', MAX_BUF);
                                        break;
                                    }
                                }
                            }else if (result == '?'){
                                print_state = 0;
                            }
                            break;
                        }
                    }
                    if (game.head != NULL && print_state == 1) {
                        broadcast(&game, status_message(msg, &game), &first);
                        announce_turn(&game, &first);
                    }
                }else {
                    // Check if any new players are entering their names
                    for (p = new_players; p != NULL; p = p->next) {
                        if (cur_fd == p->fd) {
                            // TODO - handle input from an new client who has
                            // not entered an acceptable name.
                            repeatedtively_receive_string(p);
                            if (strlen(p->inbuf) != 0 && strlen(p->name) == 0){
                                continue;
                            }
                            if (is_valid_name(game.head, p->name)){
                                join_after_enter_a_valid_name(p, &new_players, &game, &first, msg, &q, &first);
                                if (write(p->fd, status_message(msg, &game), MAX_BUF) == -1){
                                    disconnect(p, &game, &first);
                                }
                                announce_turn(&game, &first);
                            }else{
                                char question[] = "What's your name ?\r\n";
                                if (write(cur_fd, question, sizeof(question)) == -1){
                                    remove_player(&(new_players), p->fd);
                                }
                            }
                            break;
                        }
                    }
                }

            }
        }

    }
    return 0;
}



