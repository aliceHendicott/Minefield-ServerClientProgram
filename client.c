#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>

#define NUM_TILES_X 9
#define NUM_TILES_Y 9

//pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

//initialise functions
int connectToServer(char *IP_address, int socket_port_int);
bool handle_login(int sock);
int run_menu(void);
bool run_minesweeper_step(int sock);
void run_minesweeper(int sock);
void run_leaderboard(int sock);
bool run_selected_function(int menu_selection, int sock);
char run_minesweeper_menu(void);
void display_playing_field(int revealed_tiles[NUM_TILES_X][NUM_TILES_Y], int remaining_mines, bool flagged_tiles[NUM_TILES_X][NUM_TILES_Y]);
bool check_coordinates(char coordinates[2000]);
void display_mines(int mines[NUM_TILES_X][NUM_TILES_Y]);

int main(int argc , char *argv[]){
	
	char *IP_address, *socket_port;
    int socket_port_int;

    //pull IP address and port number from args
    if (argc != 3){
        printf("Must enter IP address and port\n");
        return 1;
    } else{
        IP_address = argv[1];
        socket_port = argv[2];
    }
    socket_port_int = atoi(socket_port);

    //set up connection to server socket
	int sock;
	sock = connectToServer(IP_address, socket_port_int);
	if (sock == -1){
		return 1;
	}

	//ask user to log in and authenticate
	bool logged_in;
	logged_in = handle_login(sock);
	if (!logged_in){
		return 1;
	}

	bool withinGame = true;
	while(withinGame){
		
		//run game menu
		int menu_selection;
		menu_selection = run_menu();
		printf("Your selection: %d\n\n", menu_selection);
		send(sock, &menu_selection , sizeof(int), 0);


		//run selected function
		withinGame = run_selected_function(menu_selection, sock);
		if (!withinGame){
			close(sock);
			exit(0);
		}
	}
	return 1;
}

int connectToServer(char *IP_address, int socket_port_int){
	int sock;
    struct sockaddr_in server;

	sock = socket(AF_INET , SOCK_STREAM , 0);
    if (sock == -1)
    {
        puts("Could not create socket");
        return -1;
    }
    puts("Client socket created");
     
    server.sin_addr.s_addr = inet_addr( IP_address );
    server.sin_family = AF_INET;
    server.sin_port = htons( socket_port_int );
 
    //Connect to remote server
    if (connect(sock , (struct sockaddr *)&server , sizeof(server)) < 0)
    {
        perror("connect failed. Error");
        return -1;
    }
    puts("connected");

    return sock;
}

bool handle_login(int sock){
	int read_size;
	char buffer[2000];

	printf("===============================================\n");
    printf("Welcome to the online Minesweeper gaming system\n");
    printf("===============================================\n\n");

    printf("You are required to log on with registered user name and password\n\n");

    //get username input
    char username[2000];
    printf("Username: ");
    scanf("%s", username);

    //send username input
    send(sock, username , strlen(username), 0);
    read_size = recv(sock, buffer, 2000, 0);

    //get password input
    char password[2000];
    printf("Password: ");
    scanf("%s", password);

    //send password input
    send(sock, password , strlen(password)+2, 0);
    read_size = recv(sock, buffer, 2000, 0);

    if (strstr(buffer, "true") != NULL){
    	puts("You have been authenticated\n");
    	return true;
    } else{
    	puts("You have NOT been authenticated\n");
    	return false;

    }
}

//run game menu
int run_menu(void){

	char selection;
	int int_selection;
	bool valid_selection = false;
	while(!valid_selection){
	    printf("Please enter a selection\n");
	    printf("<1> Play Minesweeper\n");
	    printf("<2> Show Leaderboard\n");
	    printf("<3> Quit\n\n");
	    printf("Selection option (1-3):");

	    scanf(" %c", &selection);

	    if (isdigit(selection)){
	    	int_selection = atoi(&selection);
	    	if (int_selection > 3 || int_selection < 1){
				puts("Please enter a valid selection\n");
				valid_selection = false;
			} else{
				valid_selection = true;
			}
	    } else{
	    	puts("Please enter a valid selection\n");
			valid_selection = false;
	    }
	}

    return int_selection;
}

//run the minesweeper menu
char run_minesweeper_menu(void){
	char selection;

	bool valid_selection = false;
	while(!valid_selection){
	    printf("Please enter a selection\n");
	    printf("<R> Reveal a tile\n");
	    printf("<P> Place a flag\n");
	    printf("<Q> Quit game\n\n");
	    printf("Selection option (R, P, Q):");

	    scanf(" %c", &selection);

	    if (strstr(&selection, "R")==NULL && strstr(&selection, "P")==NULL && strstr(&selection, "Q")==NULL){
			puts("Please enter a valid selection\n");
			valid_selection = false;
		} else{
			valid_selection = true;
		}
	}

    return selection;
}

//run function based on entered selection from game menu
bool run_selected_function(int menu_selection, int sock){
	if (menu_selection == 1){
		run_minesweeper(sock);
	} else if (menu_selection == 2){
		run_leaderboard(sock);
	} else if (menu_selection == 3){
		return false;
	}
	return true;
}

//run leaderboard by receiving from server
void run_leaderboard(int sock){
	char leaderboard_entry[2000];
	int num_entries;
	char *confirmation = "confirmation";

	printf("LEADERBOARD\n");
	printf("-----------------------------------------------------------\n");

	recv(sock, &num_entries, sizeof(int), 0);

	if (num_entries == 0){
		printf("There are currenlty no leaderboard entries\n");
	} else{
		for (int i = 0; i < num_entries; i++){
			recv(sock, leaderboard_entry, sizeof(char)*2000, 0);
			send(sock,confirmation, 2000*sizeof(char), 0);
			printf("%s\n", leaderboard_entry);
			memset(leaderboard_entry, 0, strlen(leaderboard_entry));
		}
	}

	printf("-----------------------------------------------------------\n\n");
}

//run the minesweeper game
void run_minesweeper(int sock){

	int read_size;

	bool playing_minesweeper = true;
	bool won_game = false;
	while(playing_minesweeper && !won_game){
		playing_minesweeper = run_minesweeper_step(sock);
		char ready[2000] = "ready";
		send(sock, ready, strlen(ready), 0);
		recv(sock, &won_game, sizeof(bool), 0);
	}
	if (won_game){
		time_t time_taken;
		recv(sock, &time_taken, sizeof(time_t), 0);
		printf("Congratulations you have found all the mines. You have won in %ld seconds!\n\n", time_taken);
	}
}

//display all mines on game over
void display_mines(int mines[NUM_TILES_X][NUM_TILES_Y]){

	printf("Game over, you hit a mine!\n");
	//print top row
	printf("   ");
	for (int i = 0; i < NUM_TILES_X; i++){
		printf(" %d", i);
	}
	printf("\n");
	printf("----------------------\n");

	//print the rest
	for (int i = 0; i < NUM_TILES_Y; i++){
		char letter = i + 0x41;
		printf("%c |", letter);
		for (int j = 0; j < NUM_TILES_X; j++){
			if (mines[j][i] == 1){
				printf(" *");
			}else{
				printf("  ");
			}
		}
		printf("\n");
	}

	printf("\n");
}

//display playing field based on tiles sent and remaining mines
void display_playing_field(int tiles[NUM_TILES_X][NUM_TILES_Y], int remaining_mines, bool flagged_tiles[NUM_TILES_X][NUM_TILES_Y]){
	printf("Remaining mines: %d\n\n", remaining_mines);

	//print top row
	printf("   ");
	for (int i = 0; i < NUM_TILES_X; i++){
		printf(" %d", i);
	}
	printf("\n");
	printf("----------------------\n");

	//print the rest
	for (int i = 0; i < NUM_TILES_Y; i++){
		char letter = i + 0x41;
		printf("%c |", letter);
		for (int j = 0; j < NUM_TILES_X; j++){
			if (tiles[j][i] != -1){
				printf(" %d", tiles[j][i]);
			} else if (flagged_tiles[j][i]){
				printf(" +");
			} else{
				printf("  ");
			}
		}
		printf("\n");
	}

	printf("\n");
}

//run an iteration of minesweeper
bool run_minesweeper_step(int sock){
	int read_size;
	int tiles[NUM_TILES_X][NUM_TILES_Y];

	//revieve tiles necessary
	read_size = recv(sock, tiles, (NUM_TILES_X * NUM_TILES_Y) * sizeof(int), 0);
	if (read_size < 0){
		printf("Did not receive revealed tiles\n");
	}

	//receive number of remaining tiles
	int remaining_mines;
	read_size = recv(sock, &remaining_mines, sizeof(int), 0);
	if (read_size < 0){
		printf("Did not receive revealed tiles\n");
	}

	//receive flagged tiles
	bool flagged_tiles[NUM_TILES_X][NUM_TILES_Y];
	read_size = recv(sock, flagged_tiles, (NUM_TILES_X * NUM_TILES_Y) * sizeof(bool), 0);
	if (read_size < 0){
		printf("Did not receive revealed tiles\n");
	}

	//display the playing field based on these
	display_playing_field(tiles, remaining_mines, flagged_tiles);

	char selection;
	//run the minesweeper menu
	selection = run_minesweeper_menu();
	printf("Menu selection: %c\n\n", selection);

	//if player chosen R or P, get coordinated and check they are valid
	char coordinates[2000], buffer[2000];
	if(strstr(&selection, "R")!=NULL || strstr(&selection, "P")!=NULL){
		bool coords_valid = false;
		while (!coords_valid){
			printf("Enter tile coordinates: ");
			scanf("%s", coordinates);
			coords_valid = check_coordinates(coordinates);
			if (!coords_valid){
				puts("You have not entered valid coordinates, try again");
			}
		}
		
		//send minesweeper menu selection
		printf("\n");
		if (send(sock, &selection, sizeof(char), 0) < 0){
			puts("send failed");
		}
		
		//receive confirmation of send
		read_size = recv(sock, buffer, 2000, 0);
		
		//send coordinates
		if (send(sock, coordinates, sizeof(char)*2000, 0) < 0){
			puts("send failed");
		}
		memset(coordinates,0,strlen(coordinates));

		//prints message if place a tile is not on a mine
		if (strstr(&selection, "P")!=NULL){
			read_size = recv(sock, buffer, 2000, 0);
			if (strstr(buffer, "not")!=NULL){
				printf("%s\n\n", buffer);
				return true;
			}
		} else{ //prints a message if user has hit a tile
			read_size = recv(sock, buffer, 2000, 0);
			if (strstr(buffer, "over")!=NULL){
				printf("%s\n\n", buffer);
				int mines[NUM_TILES_X][NUM_TILES_Y];
				recv(sock, mines, sizeof(int)*NUM_TILES_Y*NUM_TILES_X, 0);
				display_mines(mines);
				return false;
			} else if(strstr(buffer, "already")!=NULL){
				printf("%s\n", buffer);
				return true;
			}
		}
	
	//quits game if selected	
	} else if(strstr(&selection, "Q")!=NULL){
		puts("12");
		send(sock, &selection, sizeof(char), 0);
		puts("13");
		read_size = recv(sock, buffer, 2000, 0);
		if (read_size < 0){
			puts("read of confirmation failed");
		}
		puts("returning false");
		return false;
	}

	return true;
}

//check if coordinates are valid
bool check_coordinates(char coordinates[2000]){
	int length = strlen(coordinates);
	
	int x, y;
	char *x_char, y_char;
	x_char = &coordinates[1];
	y_char = coordinates[0];

	x = atoi(x_char);
	y = y_char - 0x41;

	if (length != 2){
		return false;
	} else if (x >= NUM_TILES_X || x < 0){
		return false;
	} else if (y >= NUM_TILES_Y || y < 0){
		return false;
	} else{
		return true;
	}
}
