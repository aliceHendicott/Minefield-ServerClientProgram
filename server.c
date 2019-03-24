#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

//declare constant global variables
#define RANDOM_NUMBER_SEED 42
#define NUM_TILES_X 9
#define NUM_TILES_Y 9
#define NUM_MINES 10

/* number of threads used to service requests */
#define NUM_HANDLER_THREADS 10

/* global mutex for our program. assignment initializes it. */
pthread_mutex_t req_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

/* global condition variable for our program. assignment initializes it. */
pthread_cond_t  got_request   = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mutex;
static pthread_mutex_t lb_mutex;

int num_requests = 0;

//set up structure for a tile on the playing field
typedef struct{
    int adjacent_mines;
    bool revealed;
    bool is_mine;
    bool is_flagged;
} Tile;


//set up structure for the game state
typedef struct {
    int num_fields_revealed;
    int num_flags;
    int num_mines_remaining;
    bool hit_mine;
    int flag_positions[NUM_MINES][2];
    Tile tiles[NUM_TILES_X][NUM_TILES_Y];
} GameState;


//set up structure for a user
typedef struct{
	char name[200];
	int num_games_won;
	int num_games_played;
} User;

//set up linked list structure for a leaderboard entry
struct leaderboard{
	User *user;
	time_t time_taken;
	struct leaderboard *next;
};

int num_leaderboard_entries = 0;

typedef struct leaderboard entry;

/* format of a single request. */
struct request {
    int number;             /* number of the request                  */
    struct request* next;   /* pointer to next request, NULL if none. */
};
struct request* req = NULL;     /* head of linked list of requests. */
struct request* last_req = NULL; /* pointer to last request.         */

entry *head = NULL; //leaderboard head entry
entry *tail = NULL; //leaderboard tail entry

//create function to insert leaderboard entry into linked list at correct position
void insert_entry(entry *new) {
	entry *p;
    //if the linked list is empty, insert the new entry as head and tail.
    if (tail == NULL ){ /* empty list */
        head = new;
        tail = new;
    } else{
      //else find appropriate point in list to insert
     	p = head;
     	entry *p_previous = NULL;
     	bool reached_spot = false;
     	do{
        //if the new entry has less time then current entry insert before
     		if(new->time_taken <= p->time_taken){
          if (new->time_taken == p->time_taken){
            if (new->user->num_games_won > p->user->num_games_won){
              continue;
            }
          }
          //if the current is the head - insert as new head
     			if (p_previous == NULL){
     				new->next = head;
     				head = new;
     				reached_spot = true;
     			} else{ //else add into the middle
     				p_previous->next = new;
     				new->next = p;
     				reached_spot = true;
     			}
     		}
        //go to next entry in list
     		p_previous = p;
     		if (p->next == NULL){
     			break;
     		} else{
     			p = p->next;
     		}
     	} while(p!=NULL && !reached_spot);
     	if (!reached_spot){
     		tail->next = new;
     		tail = new;
     	}
    }
}

//function to print leaderboard
void print_leaderboard(entry *p){
	if (p == NULL){
      printf("^\n");
  	} else {
        while(p->next) {
            printf("%s \t %ld seconds \t %d games won, %d games played\n", p->user->name, p->time_taken, p->user->num_games_won, p->user->num_games_played);
            p = p->next ;
        }
        printf("%s \t %ld seconds \t %d games won, %d games played\n", p->user->name, p->time_taken, p->user->num_games_won, p->user->num_games_played);
   	}
}

User *users;

//initialise functions
int get_num_users(void);
int setUpServer(int socket_port_int);
int connectToClient(int server_socket);
int handle_login(int client_socket);
int authenticate_user(char username[2000], char password[2000]);
void run_selected_function(int menu_selection, int client_socket, int logged_in_user);
bool run_minesweeper(int client_socket);
void run_leaderboard(int client_socket);
bool tile_contains_mine(int x, int y, GameState current_game);
GameState set_adjacent_mines(int x, int y, GameState current_game);
GameState place_mines(GameState current_game);
GameState setup_minesweeper(void);
GameState place_flag(GameState current_game, char coordinates[2000], int client_socket);
bool test_if_won(GameState current_game);
GameState reveal_tile(GameState current_game, char coordinates[2000], int client_socket,  int mines[NUM_TILES_X][NUM_TILES_Y]);
GameState test_tile(GameState current_game, int x, int y);
void *connection_handler(void *);
void sig_handler(int num);
void terminate_client(int client_socket);

void add_request(int request_num, pthread_mutex_t* p_mutex, pthread_cond_t*  p_cond_var){
    int rc;                         /* return code of pthreads functions.  */
    struct request* a_req;      /* pointer to newly added request.     */

    /* create structure with new request */
    a_req = (struct request*)malloc(sizeof(struct request));
    if (!a_req) { /* malloc failed?? */
        fprintf(stderr, "add_request: out of memory\n");
        exit(1);
    }
    a_req->number = request_num;
    a_req->next = NULL;

    /* lock the mutex, to assure exclusive access to the list */
    rc = pthread_mutex_lock(p_mutex);

    /* add new request to the end of the list, updating list */
    /* pointers as required */
    if (num_requests == 0) { /* special case - list is empty */
        req = a_req;
        last_req = a_req;
    }
    else {
        last_req->next = a_req;
        last_req = a_req;
    }

    /* increase total number of pending requests by one. */
    num_requests++;

    /* unlock mutex */
    rc = pthread_mutex_unlock(p_mutex);

    /* signal the condition variable - there's a new request to handle */
    rc = pthread_cond_signal(p_cond_var);
}

struct request* get_request(pthread_mutex_t* p_mutex)
{
    int rc;                         /* return code of pthreads functions.  */
    struct request* a_req;      /* pointer to request.                 */

    /* lock the mutex, to assure exclusive access to the list */
    rc = pthread_mutex_lock(p_mutex);

    if (num_requests > 0) {
        a_req = req;
        req = a_req->next;
        if (req == NULL) { /* this was the last request on the list */
            last_req = NULL;
        }
        /* decrease the total number of pending requests */
        num_requests--;
    }
    else { /* requests list is empty */
        a_req = NULL;
    }

    /* unlock mutex */
    rc = pthread_mutex_unlock(p_mutex);

    /* return the request to the caller. */
    return a_req;
}

void handle_request(struct request* a_req, int thread_id)
{
    if (a_req) {
        printf("Thread '%d' handled request '%d'\n", thread_id, a_req->number);
        fflush(stdout);
    }
}

void* handle_requests_loop(void* data)
{
    int rc;                         /* return code of pthreads functions.  */
    struct request* a_req;      /* pointer to a request.               */
    int thread_id = *((int*)data);  /* thread identifying number           */


    /* lock the mutex, to access the requests list exclusively. */
    rc = pthread_mutex_lock(&req_mutex);

    /* do forever.... */
    while (1) {

        if (num_requests > 0) { /* a request is pending */
            a_req = get_request(&req_mutex);
            if (a_req) { /* got a request - handle it and free it */
                /* unlock mutex - so other threads would be able to handle */
                /* other reqeusts waiting in the queue paralelly.          */
                rc = pthread_mutex_unlock(&req_mutex);
                handle_request(a_req, thread_id);
                free(a_req);
                /* and lock the mutex again. */
                rc = pthread_mutex_lock(&req_mutex);
            }
        }
        else {
            /* wait for a request to arrive. note the mutex will be */
            /* unlocked here, thus allowing other threads access to */
            /* requests list.                                       */

            rc = pthread_cond_wait(&got_request, &req_mutex);
            /* and after we return from pthread_cond_wait, the mutex  */
            /* is locked again, so we don't need to lock it ourselves */

        }
    }
}

int main(int argc , char *argv[]){
  int        i;                                /* loop counter          */
  int        thr_id[NUM_HANDLER_THREADS];      /* thread IDs            */
  pthread_t  p_threads[NUM_HANDLER_THREADS];   /* thread's structures   */

  /* create the request-handling threads */
  for (i=0; i<NUM_HANDLER_THREADS; i++) {
      thr_id[i] = i;
      pthread_create(&p_threads[i], NULL, handle_requests_loop, (void*)&thr_id[i]);
  }

	//pthreads_mutex_lock(&mutex);
	srand(RANDOM_NUMBER_SEED);
	//pthreads_mutex_unlock(&mutex);

	signal(SIGINT,sig_handler);

	pthread_mutex_init(&lb_mutex, NULL);

	//pull port to run server on from args
	int socket_port_int;
	char *socket_port;
	if (argc == 1){
    socket_port_int = 12345;
  } else{
    socket_port = argv[1];
    socket_port_int = atoi(socket_port);
  }

  //set up the server socket
	int server_socket;
	server_socket = setUpServer(socket_port_int);
	if (server_socket == -1){
		return -1;
	}

  //set up user structures
	int num_users = get_num_users();
	User users_array[num_users];
	users = users_array;
	printf("Number of users: %d\n", num_users);
	//initialise users from authentication.txt file
	for (int i = 0; i < num_users; i++){
			FILE *fp = fopen("Authentication.txt", "r");
			char c, username[200];
			int lineNum = -1;
			int index = 0;
			bool on_username = true;
			while (c != EOF && lineNum <= i){
				if (lineNum == i && on_username){
					username[index] = c;
					index++;
				}
				if (c == '\n'){
					lineNum++;
					on_username = true;
				}
				if ((c == ' ' || c == '\t') && on_username){
					on_username = false;
				}
				c = fgetc(fp);
			}
			fclose(fp);
			strcpy(users[i].name, username);
			users[i].num_games_played = 0;
			users[i].num_games_won = 0;
			memset(username,0,strlen(username));
	}

    // /* create the request-handling threads */
    // for (i=0; i<NUM_HANDLER_THREADS; i++) {
    //     thr_id[i] = i;
    //     pthread_create(&p_threads[i], NULL, connection_handler, (void*)&thr_id[i]);
    // }

	while (1){
		//connect to client socket
    int client_socket;
		client_socket = connectToClient(server_socket);
		if (client_socket == -1){
			return -1;
		}
	}

	return 1;
}

int setUpServer(int socket_port_int){
	int socket_desc;
    struct sockaddr_in server;

    //Create socket
    socket_desc = socket(AF_INET , SOCK_STREAM , 0);
    if (socket_desc == -1)
    {
        printf("Could not create socket");
        return -1;
    }
    printf("Socket created");

    //Prepare the sockaddr_in structure - assign IP address and port
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    server.sin_port = htons( socket_port_int );

    //Bind socket to server
    if( bind(socket_desc,(struct sockaddr *)&server , sizeof(server)) < 0)
    {
        //print the error message
        perror("bind failed. Error");
        return -1;
    }
    printf("Socket binded");

    //display server IP address and port to screen
    printf("Server is running on IP address: %s\n", inet_ntoa(server.sin_addr));
    printf("Server is running on port: %d\n", (int) ntohs(server.sin_port));

    return socket_desc;
}

int connectToClient(int server_socket){
	int client_socket, c;
    struct sockaddr_in client;

	   listen(server_socket , 3);
    //puts("Please run the client in another terminal...");

    //Accept an incoming connection
    c = sizeof(struct sockaddr_in);

    puts("Please run the client in another terminal...");

    //Accept an incoming connection
    c = sizeof(struct sockaddr_in);
    pthread_t thread_id;

    while( (client_socket = accept(server_socket, (struct sockaddr *)&client, (socklen_t*)&c)) ){
    	puts("Connected accepted");
		  add_request(10, &req_mutex, &got_request);
    	if (pthread_create(&thread_id, NULL, connection_handler, (void*)&client_socket) < 0){
    		perror("could not create socket");
    		return -1;
    	}
    	puts("Hanfler assigned");
    }

    //accept connection from an incoming client
    if (client_socket < 0)
    {
        perror("accept failed");
        return -1;
    }

    return client_socket;
}

void *connection_handler(void *socket_desc){
  // int rc;                         /* return code of pthreads functions.  */
  // struct request* a_req;      /* pointer to a request.               */
  int client_socket = *(int*)socket_desc;
  int read_size;
  int menu_selection;

	//log in user
	int logged_in_user;
	logged_in_user = handle_login(client_socket);
	User current_user = users[logged_in_user];
	printf("Logged in user: %s\n", current_user.name);

	bool withinGame = true;
	while(withinGame){

  // if (num_requests > 0) {
    read_size = recv(client_socket, &menu_selection, sizeof(int), 0);
    if (read_size <= 0){
      break;
    }
    // a_req = get_request(&req_mutex);
    // if (a_req) { /* got a request - handle it and free it */
    //     /* unlock mutex - so other threads would be able to handle */
    //     /* other reqeusts waiting in the queue paralelly.          */
    //     rc = pthread_mutex_unlock(&req_mutex);
    //     // handle_request(a_req, thread_id);
    //     free(a_req);
    //     /* and lock the mutex again. */
    //     rc = pthread_mutex_lock(&req_mutex);
    // }
		printf("Menu selection: %d\n\n", menu_selection);
		if (menu_selection == 3){
			withinGame = false;
		}
  // }
    // else {
    //
    //     rc = pthread_cond_wait(&got_request, &req_mutex);
        run_selected_function(menu_selection, client_socket, logged_in_user);
    		menu_selection = 0;
    // }

	}
	return 0;
};

int get_num_users(void){
	FILE *fp = fopen("Authentication.txt", "r");
	char c;
	int numLines = 0;

	while(c != EOF){
		if (c == '\n'){
			numLines++;
		}
		c = fgetc(fp);
	}

	fclose(fp);
	return numLines - 1;
}

int handle_login(int client_socket){
	int read_size;
	char buffer_username[2000], buffer_password[2000];
	char *confirmation = "received";

	//receive username and send confirmation
	read_size = recv(client_socket, buffer_username, 2000, 0);
	printf("Username: %s\n", buffer_username);
	send(client_socket, confirmation, strlen(confirmation), 0);

	//receive password
	read_size = recv(client_socket, buffer_password, 2000, 0);
	printf("Password: %s\n", buffer_password);

	//authenticate user
	int authenticated;
	authenticated = authenticate_user(buffer_username, buffer_password);

	//tell client if user is authenticated
	if (authenticated > -1){
		confirmation = "true";
	}
	send(client_socket, confirmation, strlen(confirmation), 0);

	return authenticated;
}

int authenticate_user(char username[2000], char password[2000]){
	FILE *fp;
	char filename[100], c;
	char file_username[2000];
	char file_password[2000];
	bool correct = false;
	int lineNum = -1;

	// Open one file for reading
	fp = fopen("Authentication.txt", "r");
	//Reading file and comparing username and password
	while (correct == false && c != EOF){
		 if (c == '\n'){
  			lineNum++;
  		}
		fscanf(fp, "%s %s", file_username, file_password);
		if(strcmp(file_username,username) == 0 && strcmp(file_password,password) == 0){
  			puts("login successful");
  			correct = true;
  			break;
  		}
		c = fgetc(fp);
	}

	if(!correct){
		puts("incorrect login");
		return -1;
	}
	return lineNum;
}

void run_selected_function(int menu_selection, int client_socket, int logged_in_user){

  //run function based on selected menu option
	if (menu_selection == 1){
    //start timer for game
		time_t begin, end, time_spent;
		begin = time(NULL);
    //run game
		bool won_game = run_minesweeper(client_socket);
		end = time(NULL);
    //calculate time
		time_spent = (end - begin);
		users[logged_in_user].num_games_played++;
    //if user won - insert entry to leaderboard
		if (won_game){
			users[logged_in_user].num_games_won++;
			entry *p = (entry *)malloc(sizeof(entry));
			p->user = &users[logged_in_user];
			p->time_taken = time_spent;
			p->next = NULL;
			pthread_mutex_lock(&lb_mutex);
			insert_entry(p);
			num_leaderboard_entries++;
			pthread_mutex_unlock(&lb_mutex);
			print_leaderboard(head);
      send(client_socket, &time_spent, sizeof(time_t), 0);
		}
	} else if (menu_selection == 2){
		run_leaderboard(client_socket);
	} else if (menu_selection == 3){
		close(client_socket);
	}
}

//run minesweeper function
bool run_minesweeper(int client_socket){
  //setup game
	GameState current_game = setup_minesweeper();

	bool quit_game = false;
	bool hit_mine = false;
	bool won_game = false;

	int adjacent_mines[NUM_TILES_X][NUM_TILES_Y];
	for (int i = 0; i < NUM_TILES_X; i++){
		for (int j = 0; j < NUM_TILES_Y; j++){
			adjacent_mines[i][j] = current_game.tiles[i][j].adjacent_mines;
		}
	}

  int mines[NUM_TILES_X][NUM_TILES_Y];
  for (int i = 0; i < NUM_TILES_X; i++){
    for (int j = 0; j < NUM_TILES_Y; j++){
      if(current_game.tiles[i][j].is_mine){
        mines[i][j] = 1;
      } else{
        mines[i][j] = 0;
      }
    }
  }

  //play game until user quits, hits a mine or wins
	while(!quit_game && !hit_mine && !won_game){
    //determine tiles to send based on revealed tiles
		int tiles_to_send[NUM_TILES_X][NUM_TILES_Y];
		for (int i = 0; i < NUM_TILES_X; i++){
			for (int j = 0; j < NUM_TILES_Y; j++){
				if (current_game.tiles[i][j].revealed){
					tiles_to_send[i][j] = adjacent_mines[i][j];
				} else{
					tiles_to_send[i][j] = -1;
				}
			}
		}

    //send tiles necessary
		if (send(client_socket, tiles_to_send, (NUM_TILES_Y * NUM_TILES_X) * sizeof(int), 0) < 0){
			puts("send of revealed tiles failed");
		}

    //send number of remaining mines
		int remaining_mines = current_game.num_mines_remaining;
		if (send(client_socket, &remaining_mines, sizeof(int), 0) < 0){
			puts("send of remaining_mines failed");
		}

    //send flagged tiles
		bool flagged_tiles[NUM_TILES_X][NUM_TILES_Y];
		for (int i = 0; i < NUM_TILES_X; i++){
			for (int j = 0; j < NUM_TILES_Y; j++){
				flagged_tiles[i][j] = current_game.tiles[i][j].is_flagged;
			}
		}
		if (send(client_socket, flagged_tiles, (NUM_TILES_Y * NUM_TILES_X) * sizeof(bool), 0) < 0){
			puts("send of flagged tiles failed");
		}

    //receive menu selection
		char selection;
		recv(client_socket, &selection, sizeof(char), 0);
		printf("%c\n", selection);

		char *confirmation = "received";
		if (strstr(&selection, "Q")!=NULL){
			quit_game = true;
			//send(client_socket, confirmation, strlen(confirmation), 0);
		}
		if (send(client_socket, confirmation, strlen(confirmation), 0) < 0){
			puts("failed confirmation");
		}

		char coordinates[2000];

    //run function based on selection
		if(strstr(&selection, "R")!=NULL){
			recv(client_socket, coordinates, sizeof(char)*2000, 0);
			printf("%s\n", coordinates);
			current_game = reveal_tile(current_game, coordinates, client_socket, mines);
			hit_mine = current_game.hit_mine;
		} else if(strstr(&selection, "P")!=NULL){
			recv(client_socket, coordinates, sizeof(char)*2000, 0);
			printf("%s\n", coordinates);
			current_game = place_flag(current_game, coordinates, client_socket);
			won_game = test_if_won(current_game);
		}
		char ready[2000];
		recv(client_socket, ready, sizeof(char)*2000, 0);
		send(client_socket, &won_game, sizeof(bool), 0);
	}
	return won_game;
}

//function to test if the user has found all mines
bool test_if_won(GameState current_game){
	if (current_game.num_mines_remaining == 0){
		return true;
	} else{
		return false;
	}
}

GameState reveal_tile(GameState current_game, char coordinates[2000], int client_socket,  int mines[NUM_TILES_X][NUM_TILES_Y]){
	int x, y;
	char *x_char, y_char;

  //convert entered coordinates to integars
	x_char = &coordinates[1];
	y_char = coordinates[0];

	x = atoi(x_char);
	printf("x: %d\n", x);
	y = y_char - 0x41;
	printf("y: %d\n", y);

	char *confirmation;

  //choose whether tile should be revelaed and reveal all other necessary tiles
	if (current_game.tiles[x][y].is_mine){
		confirmation = "Game over! You have hit a mine";
		current_game.hit_mine = true;
	} else if (current_game.tiles[x][y].revealed){
		confirmation = "This tile has already been revealed, try again.";
	} else{
		current_game = test_tile(current_game, x, y);
		confirmation = "Tiles revealed";
	}
	send(client_socket, confirmation, strlen(confirmation), 0);

  if (strstr(confirmation, "over")!=NULL){
    send(client_socket, mines, sizeof(int)*NUM_TILES_Y*NUM_TILES_X, 0);
  }

	return current_game;
}

//test all border tiles if they need to be revealed
GameState test_tile(GameState current_game, int x, int y){
	if (current_game.tiles[x][y].is_mine){
		return current_game;
	}
	if(current_game.tiles[x][y].revealed){
		return current_game;
	} else if (current_game.tiles[x][y].adjacent_mines > 0){
		current_game.tiles[x][y].revealed = true;
		return current_game;
	} else{
		current_game.tiles[x][y].revealed = true;
		if (x+1 < NUM_TILES_X){
			current_game = test_tile(current_game, x+1, y);
			if (y-1 >= 0){
				current_game = test_tile(current_game, x+1, y-1);
			}
			if (y+1 < NUM_TILES_Y){
				current_game = test_tile(current_game, x+1, y+1);
			}
		}
		if (y+1 < NUM_TILES_Y){
			current_game = test_tile(current_game, x, y+1);
		}
		if (y-1 >= 0){
			current_game = test_tile(current_game, x, y-1);
		}
		if (x-1 >= 0){
			current_game = test_tile(current_game, x-1, y);
			if (y+1 < NUM_TILES_Y){
				current_game = test_tile(current_game, x-1, y+1);
			}
			if (y-1 >= 0){
				current_game = test_tile(current_game, x-1, y-1);
			}
		}
		return current_game;
	}
}

//function to place a flag
GameState place_flag(GameState current_game, char coordinates[2000], int client_socket){

	int x, y;
	char *x_char, y_char;

	x_char = &coordinates[1];
	y_char = coordinates[0];

	x = atoi(x_char);
	printf("x: %d\n", x);
	y = y_char - 0x41;
	printf("y: %d\n", y);

	char *confirmation;

	if (current_game.tiles[x][y].is_mine && current_game.tiles[x][y].is_flagged == false){
		current_game.tiles[x][y].is_flagged = true;
		current_game.num_mines_remaining--;
		confirmation = "You have found a mine";
	} else{
		confirmation = "This is not a mine, try again.";
	}
	send(client_socket, confirmation, strlen(confirmation), 0);

	return current_game;
}

//run the leaderboard function
void run_leaderboard(int client_socket){
	printf("Running leaderboard\n");
	char leaderboard_string[2000];
	char confirmation[80];
	entry *p;
	p = head;
  //send number of entries in the leaderboard
	send(client_socket, &num_leaderboard_entries, sizeof(int), 0);
	pthread_mutex_lock(&mutex);
	if (p == NULL){
      printf("^\n");
	     pthread_mutex_unlock(&mutex);
  } else {
    //send all lines in leaderboard
    while(p->next) {
            snprintf(leaderboard_string, 2000*sizeof(char), "%s%s \t\t %ld seconds \t %d games won, %d games played\n", leaderboard_string, p->user->name, p->time_taken, p->user->num_games_won, p->user->num_games_played);
            send(client_socket, leaderboard_string, strlen(leaderboard_string), 0);
            recv(client_socket, confirmation, 2000*sizeof(char), 0);
            memset(leaderboard_string, 0, strlen(leaderboard_string));
            p = p->next ;
        }
        snprintf(leaderboard_string, 2000*sizeof(char), "%s%s \t\t %ld seconds \t %d games won, %d games played\n", leaderboard_string, p->user->name, p->time_taken, p->user->num_games_won, p->user->num_games_played);
        send(client_socket, leaderboard_string, strlen(leaderboard_string), 0);
        recv(client_socket, confirmation, 2000*sizeof(char), 0);
        memset(leaderboard_string, 0, strlen(leaderboard_string));
   	}

	pthread_mutex_unlock(&mutex);
}

//check if a tile contains a mine
bool tile_contains_mine(int x, int y, GameState current_game){
    if (current_game.tiles[x][y].is_mine == true){
        return true;
    } else{
        return false;
    }
}

//set up adjacent mines based on position of mines
GameState set_adjacent_mines(int x, int y, GameState current_game){
    if ((x+1) < NUM_TILES_X){
            current_game.tiles[x+1][y].adjacent_mines++;
            if ((y-1) >= 0){
                current_game.tiles[x+1][y-1].adjacent_mines++;
            }
            if ((y+1) < NUM_TILES_Y){
                current_game.tiles[x+1][y+1].adjacent_mines++;
            }
        }
        if ((y+1) < NUM_TILES_Y){
            current_game.tiles[x][y+1].adjacent_mines++;
        }
        if ((y-1) >= 0){
            current_game.tiles[x][y-1].adjacent_mines++;
        }
        if ((x-1) >= 0){
            current_game.tiles[x-1][y].adjacent_mines++;
            if ((y-1) >= 0){
                current_game.tiles[x-1][y-1].adjacent_mines++;
            }
            if ((y+1) < NUM_TILES_Y){
                current_game.tiles[x-1][y+1].adjacent_mines++;
            }
        }
        return current_game;
}

//randomly place mines
GameState place_mines(GameState current_game){
    for (int i = 0; i< NUM_MINES; i++){
        int x, y;
        do {
			//pthreads_mutex_lock(&mutex);
            x = rand() % NUM_TILES_X;
            y = rand() % NUM_TILES_Y;
			//pthreads_mutex_unlock(&mutex);
        } while (tile_contains_mine(x, y, current_game));
        current_game.tiles[x][y].is_mine = true;
        current_game = set_adjacent_mines(x, y, current_game);
        printf("Mine at: (x, y) = (%d, %d)\n", x, y);
    }
    return current_game;
}

GameState setup_minesweeper(void){
    //initialise Game
    GameState current_game = {.num_fields_revealed = 0, .num_flags = 0, .num_mines_remaining = NUM_MINES, .hit_mine = false};
    for (int i = 0; i<NUM_TILES_X; i++){
        for (int j = 0; j<NUM_TILES_Y; j++){
            current_game.tiles[i][j].is_mine = false;
            current_game.tiles[i][j].revealed = false;
        }
    }
    //place 10 mines randomly
   	puts("placing mines");
    current_game = place_mines(current_game);

    return current_game;
}

void sig_handler(int num){
	printf("\n Ctrl + c detectected, initialising client termination \n");
	int socket_desc;
	terminate_client(socket_desc);
	exit(1);
}

void terminate_client(int client_socket){
	printf("\nDisconnecting client\n");
	close(client_socket);
	memset(&client_socket,0,sizeof(client_socket));
}
