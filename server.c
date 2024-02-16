#include <string.h>  
#include <unistd.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h> 
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <errno.h>

#define SA struct sockaddr 
#define BACKLOG 0 
#define PORT "8000" 
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


struct game_user_details{
	int userid;
	char ipaddr[INET6_ADDRSTRLEN];
	int connfd;
	int ingame;
	char movename;
	char board[3][3];
	struct game_user_details* next;
}*user_details;

pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;


int send_websocket_frame(int client_socket, uint8_t fin, uint8_t opcode, char *payload);
void updateDetails(int userid1,int userid2);
void activeuserssend();

void *get_in_addr(struct sockaddr *sa){
	if(sa->sa_family == AF_INET){
		return &(((struct sockaddr_in*)sa)->sin_addr);	
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


void generate_random_mask(uint8_t *mask) {
    srand(time(NULL));

    for (size_t i = 0; i < 4; ++i) {
        mask[i] = rand() & 0xFF;
    }
}


void mask_payload(uint8_t *payload, size_t payload_length, uint8_t *mask) {
    for (size_t i = 0; i < payload_length; ++i) {
        payload[i] ^= mask[i % 4];
    }
}


int encode_websocket_frame(
    uint8_t fin,
    uint8_t opcode,
    uint8_t mask,
    uint64_t payload_length,
    uint8_t *payload,
    uint8_t *frame_buffer
) {
    int header_size = 2;
    if (payload_length <= 125) {
       
    } else if (payload_length <= 65535) {
        header_size += 2;
    } else {
        header_size += 8;
    }

    frame_buffer[0] = (fin << 7) | (opcode & 0x0F);
    frame_buffer[1] = mask << 7;
    if (payload_length <= 125) {
        frame_buffer[1] |= payload_length;
    } else if (payload_length <= 65535) {
        frame_buffer[1] |= 126;
        frame_buffer[2] = (payload_length >> 8) & 0xFF;
        frame_buffer[3] = payload_length & 0xFF;
    } else {
        frame_buffer[1] |= 127;
        uint64_t n = payload_length;
        for (int i = 8; i >= 1; --i) {
            frame_buffer[i + 1] = n & 0xFF;
            n >>= 8;
        }
    }

    if (mask) {
        generate_random_mask(frame_buffer + header_size - 4);
        mask_payload(payload, payload_length, frame_buffer + header_size - 4);
    }

    memcpy(frame_buffer + header_size, payload, payload_length);

    return header_size + payload_length; 
}

int decode_websocket_frame_header(
    uint8_t *frame_buffer,
    uint8_t *fin,
    uint8_t *opcode,
    uint8_t *mask,
    uint64_t *payload_length
) {
    *fin = (frame_buffer[0] >> 7) & 1;
    *opcode = frame_buffer[0] & 0x0F;
    *mask = (frame_buffer[1] >> 7) & 1;
    int n = 0;
    

    *payload_length = frame_buffer[1] & 0x7F;
    if (*payload_length == 126) {
        n = 1;
        *payload_length = *(frame_buffer + 2);
        *payload_length <<= 8;
        *payload_length |= *(frame_buffer + 3);
    } else if (*payload_length == 127) {
        n = 2;
        *payload_length = 0;
        for (int i = 2; i < 10; ++i) {
            *payload_length = (*payload_length << 8) | *(frame_buffer + i);
        }
    }

    return  (2 + (n == 1 ? 2 : (n == 2 ? 8 : 0)));
}


void handle_ping(const uint8_t *data, size_t length,int connfd) {

    if (length >= 1 && data[0] == 0x9) {
        // Send a pong frame in response
        uint8_t pong_frame[2] = {0x8A, 0x00};  
        ssize_t bytes_sent = send(connfd, pong_frame, sizeof(pong_frame), 0);

        if (bytes_sent == -1) {
            perror("Send failed");
        } else {
            printf("Pong Frame sent to client\n");
        }
    }
}

void handle_close(int connfd) {

    struct game_user_details* head = user_details;
    struct game_user_details* current = head;
    struct game_user_details* prev = NULL;
    while (current != NULL) {
        if(connfd == current->userid){
        	 uint8_t close_frame[] = {0x88, 0x00};
		 send(connfd, close_frame, sizeof(close_frame), 0);
		 if(current->ingame != 0){
		        char arr[100];
		        sprintf(arr,"gameOver => %d",current->ingame);
		 	if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
			updateDetails(current->userid,current->ingame);
			
		}
		break;
        }
        prev = current;
        current = current->next;
    }
    
    if(prev == NULL){
    	
    	user_details = user_details->next;
    }
    else{
    	prev->next = prev->next->next;
    }
    activeuserssend();
    pthread_mutex_unlock( &mutex1 );
    pthread_exit(NULL);
}


int process_websocket_frame(uint8_t *data, size_t length, char **decoded_data,int connfd,int* flag) {

    uint8_t fin, opcode, mask;
    uint64_t payload_length;
    uint8_t* masking_key;

    int header_size = decode_websocket_frame_header(data, &fin, &opcode, &mask, &payload_length);
    if (header_size == -1) {
        printf("Error decoding WebSocket frame header\n");
        return -1;
    }
    
    if(mask){
    	masking_key = header_size + data;
    	header_size += 2;
    }
    
    header_size += 2;
    
    size_t payload_offset = header_size;
    
    
    if (opcode == 0x9) {
        handle_ping(data,length,connfd);
        *decoded_data = NULL;
        *flag = 1;
        return 0;
    } else if (opcode == 0x8) {
    	printf("closes the connection\n");
    	pthread_mutex_lock( &mutex1 );
        handle_close(connfd);
    }

    *decoded_data = (char *)malloc(payload_length + 1);
   

    
    if(mask)
    	for (size_t i = 0; i < payload_length; ++i) {
	     (*decoded_data)[i] = data[payload_offset + i] ^ masking_key[i % 4];
	}

    (*decoded_data)[payload_length] = '\0';

    return 0;
}

int send_websocket_frame(int client_socket, uint8_t fin, uint8_t opcode, char *payload) {

    uint8_t encoded_data[1024];
    int encoded_size = encode_websocket_frame(fin, opcode, 0, strlen(payload), ( uint8_t *)payload, encoded_data);


    ssize_t bytes_sent = send(client_socket, encoded_data, encoded_size, 0);
    if (bytes_sent == -1) {
        perror("Send failed");
        return -1;
    }

    printf("Message sent to client\n");

    return 0;
}


void calculate_websocket_accept( char *client_key, char *accept_key) {
    char combined_key[1024];
    strcpy(combined_key, client_key);
    strcat(combined_key, MAGIC_STRING);

    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1(( unsigned char *)combined_key, strlen(combined_key), sha1_hash);

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    BIO *bio = BIO_new(BIO_s_mem());
    BIO_push(b64, bio);

    BIO_write(b64, sha1_hash, SHA_DIGEST_LENGTH);
    BIO_flush(b64);

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);

    strcpy(accept_key, bptr->data);


    size_t len = strlen(accept_key);
    if (len > 0 && accept_key[len - 1] == '\n') {
        accept_key[len - 1] = '\0';
    }

    BIO_free_all(b64);
}


void handle_websocket_upgrade(int client_socket, char *request) {

    if (strstr(request, "Upgrade: websocket") == NULL) {
        fprintf(stderr, "Not a WebSocket upgrade request\n");
        return;
    }

    char *key_start = strstr(request, "Sec-WebSocket-Key: ") + 19;
    char *key_end = strchr(key_start, '\r');
    
    if (!key_start || !key_end) {
        fprintf(stderr, "Invalid Sec-WebSocket-Key header\n");
        return;
    }
    *key_end = '\0';

    char accept_key[1024];
    calculate_websocket_accept(key_start, accept_key);

     char *upgrade_response_format =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n";

    char response[2048];
    sprintf(response, upgrade_response_format, accept_key);
    send(client_socket, response, strlen(response), 0);

    printf("WebSocket handshake complete\n");
}

// this is the code for server creation. here i have used TCP instead of UDP because i need all the data without any loss. if we use UDP we
// have to implement those in the upper layers.
// this function will return socket descripter to the calling function.
int server_creation(){
	int sockfd;
	struct addrinfo hints,*servinfo,*p;
	int yes = 1;
	int rv;
	memset(&hints,0,sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;// my ip
	
	// set the address of the server with the port info.
	if((rv = getaddrinfo(NULL,PORT,&hints,&servinfo)) != 0){
		fprintf(stderr, "getaddrinfo: %s\n",gai_strerror(rv));	
		return 1;
	}
	
	// loop through all the results and bind to the socket in the first we can
	for(p = servinfo; p!= NULL; p=p->ai_next){
		sockfd=socket(p->ai_family,p->ai_socktype,p->ai_protocol);
		if(sockfd==-1){ 
			perror("server: socket\n"); 
			continue; 
		} 
		
		// SO_REUSEADDR is used to reuse the same port even if it was already created by this.
		// this is needed when the program is closed due to some system errors then socket will be closed automaticlly after few
		// minutes in that case before the socket is closed if we rerun the program then we have use the already used port
		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
			perror("setsockopt");
			exit(1);	
		}
		    	
		// it will help us to bind to the port.
		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("server: bind");
			continue;
		}
		break;
	}
	
	// server will be listening with maximum simultaneos connections of BACKLOG
	if(listen(sockfd,BACKLOG) == -1){ 
		perror("listen");
		exit(1); 
	} 
	return sockfd;
}


//connection establishment with the client
//return connection descriptor to the calling function
int connection_accepting(int sockfd){
	int connfd;
	struct sockaddr_storage their_addr;
	struct game_user_details* user_detail = (struct game_user_details*)malloc(sizeof(struct game_user_details));
	char s[INET6_ADDRSTRLEN];
	socklen_t sin_size;
	
	sin_size = sizeof(their_addr); 
	connfd=accept(sockfd,(SA*)&their_addr,&sin_size); 
	if(connfd == -1){ 
		perror("\naccept error\n");
		return -1;
	} 


	inet_ntop(their_addr.ss_family,get_in_addr((struct sockaddr *)&their_addr),s, sizeof(s));
	printf("\nserver: got connection from %s\n", s);
	
        char buffer[2048];
        ssize_t len = recv(connfd, buffer, sizeof(buffer), 0);
        if (len > 0) {
            buffer[len] = '\0';
            handle_websocket_upgrade(connfd, buffer);
        }

        pthread_mutex_lock( &mutex1 );
	user_detail->connfd = connfd;
	strcpy(user_detail->ipaddr,s);
	user_detail->ingame = 0;
	user_detail->next = user_details;
	user_details = user_detail;
	
	return connfd;
}


char* extractActiveUsersString(struct game_user_details* head,int userid) {
    if (head == NULL) {
        return strdup("activeUsers  => ");
    }
    int length = 17;
    struct game_user_details* current = head;
    while (current != NULL) {
        if(userid != current->userid && !current->ingame){
        	length += 6;
        }
        current = current->next;
    }
    char* result = (char*)malloc(length + 10);
    if (result == NULL) {
        fprintf(stderr, "Memory allocation failed.\n");
        exit(EXIT_FAILURE);
    }

    current = head;
    char* pos = result;
    pos += snprintf(pos,length,"activeUsers => ");
    while (current != NULL) {
     
        if(userid != current->userid && !(current->ingame)){
        	pos += snprintf(pos, length + 1, "%d, ", current->userid);
        }
        current = current->next;
    }
    
     if (length == 17) {
        return strdup("activeUsers => ");
    }

    if (length > 17) {
        *(pos - 2) = '\0';
    }
   
    return result;
}


void activeuserssend(){
	struct game_user_details* head = user_details;
	struct game_user_details* current = head;
	
	while(current!=NULL){
		 if (send_websocket_frame(current->connfd, 1, 1, extractActiveUsersString(user_details,current->userid)) != 0) {
		   	printf("Error sending WebSocket frame\n");
	    	 }
		 current = current->next;
	}
}

void handleGameRequest(int userid,struct game_user_details* head,int RequestUserid){
    struct game_user_details* current = head;
    char arr[100];
    sprintf(arr,"gameRequest => %d",RequestUserid);
    while (current != NULL) {
        if(userid == current->userid){
        	if (send_websocket_frame(current->connfd, 1, 1, arr) != 0) {
		    printf("Error sending WebSocket frame\n");
		}
        }
        current = current->next;
    }
    
}


char checkWin(char board[3][3])
{
    for(int i=0;i<3;i++)
    {
        if(board[i][0] != ' ' && board[i][0] == board[i][1] && board[i][1] == board[i][2])
            return 1;
        if(board[0][i] != ' ' && board[0][i] == board[1][i] && board[1][i] == board[2][i])
            return 1;
    }
    if(board[0][0] != ' ' && board[0][0] == board[1][1] && board[1][1] == board[2][2])
        return 1;
    if(board[0][2] != ' ' && board[0][2] == board[1][1] && board[1][1] == board[2][0])
        return 1;
    return 0;
}

int checkDraw(char board[3][3])
{
    for(int i=0;i<3;i++)
    {
		for(int j=0;j<3;j++)
		{
		    if(board[i][j] == ' ')
		        return 0;
		}
    }
    return 1;
}


void handleacceptGame(int userid,struct game_user_details* head,int acceptUserid){
    struct game_user_details* current = head;
    char arr[100];
    sprintf(arr,"gameStart => %d, o",acceptUserid);
    int flag = 0;
    struct game_user_details* accepter = NULL;
    pthread_mutex_lock( &mutex1 );
    while (current != NULL) {
        if(userid == current->userid && current->ingame == 0){
        	if (send_websocket_frame(current->connfd, 1, 1, arr) != 0) {
		    printf("Error sending WebSocket frame\n");
		}
		current->ingame = acceptUserid;
		current->movename = 'o';
		flag = 1;
		
		sprintf(arr,"OpponentTurn");
		if(send_websocket_frame(current->connfd,1,1, arr) != 0){
			printf("Error sending WebSocket frame\n");
		}
        }
        if(acceptUserid == current->userid){
        	accepter = current;
        }
        current = current->next;
    }
    
    if(flag){
    	sprintf(arr,"gameStart => %d, x",userid);
    	accepter->ingame = userid;
    	accepter->movename = 'x';
    	if (send_websocket_frame(acceptUserid, 1, 1, arr) != 0) {
		printf("Error sending WebSocket frame\n");
	}
	sprintf(arr,"yourTurn");
	if(send_websocket_frame(acceptUserid,1,1, arr) != 0){
		printf("Error sending WebSocket frame\n");
	}
    }
    activeuserssend();
    pthread_mutex_unlock( &mutex1 );
}

void updateboard(int userid1,int userid2,int i,int j,char movename){
	struct game_user_details* current = user_details;
	
	while(current != NULL){
		if(userid1 == current->userid){
			current->board[i][j] = movename;
		}
		else if(userid2 == current->userid){
			current->board[i][j] = movename;
		}
		current = current->next;
	}
}

void updateDetails(int userid1,int userid2){
	struct game_user_details* current = user_details;
	
	while(current != NULL){
		if(userid1 == current->userid){
			for(int i=0;i<3;i++){
			    	for(int j=0;j<3;j++){
			    		current->board[i][j] = ' ';
			    	}
		    	}
		    	
		    	current->ingame = 0;
		}
		if(userid2 == current->userid){
			for(int i=0;i<3;i++){
			    	for(int j=0;j<3;j++){
			    		current->board[i][j] = ' ';
			    	}
		   	}
		   	current->ingame = 0;
		}
		current = current->next;
	}
}

void handleGamemove(int move,struct game_user_details* head,int senderUserid){

    struct game_user_details* current = user_details;
    char arr[100];
    int i = move/3,j = move%3;
    
    while (current != NULL) {
    	
        if(senderUserid == current->userid){
        	pthread_mutex_lock( &mutex1 );
        	updateboard(senderUserid,current->ingame,i,j,current->movename);
        	pthread_mutex_unlock( &mutex1 );
        	if(checkWin(current->board)){
        		sprintf(arr,"gameMove => %d",move);
			if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
		
        		sprintf(arr,"gameOver => %d",senderUserid);
        		if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
			if (send_websocket_frame(current->connfd, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
			pthread_mutex_lock( &mutex1 );
        		updateDetails(senderUserid,current->ingame);
        		activeuserssend();
        		pthread_mutex_unlock( &mutex1 );
        		break;
        	}
        	if(checkDraw(current->board)){
        	
        		sprintf(arr,"gameMove => %d",move);
			if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
        	
        		sprintf(arr,"gameOver => %d",0);
        		if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
			if (send_websocket_frame(current->connfd, 1, 1, arr) != 0) {
			    printf("Error sending WebSocket frame\n");
			}
			pthread_mutex_lock( &mutex1 );
        		updateDetails(senderUserid,current->ingame);
        		activeuserssend();
        		pthread_mutex_unlock( &mutex1 );
        		break;
        	}
		sprintf(arr,"gameMove => %d",move);
                
        	if (send_websocket_frame(current->ingame, 1, 1, arr) != 0) {
		    printf("Error sending WebSocket frame\n");
		}
		
		sprintf(arr,"yourTurn");
		if(send_websocket_frame(current->ingame,1,1, arr) != 0){
			printf("Error sending WebSocket frame\n");
		}
		
		sprintf(arr,"OpponentTurn");
		if(send_websocket_frame(current->connfd,1,1, arr) != 0){
			printf("Error sending WebSocket frame\n");
		}
		printf("hello\n");
		break;
        }
        current = current->next;
    }
    
}

void handleEndGame(struct game_user_details* userdetail,int userid){
	char arr[100];		
        sprintf(arr,"gameOver => %d",userdetail->ingame);
        if (send_websocket_frame(userdetail->ingame, 1, 1, arr) != 0) {
		printf("Error sending WebSocket frame\n");
	}
	if (send_websocket_frame(userdetail->connfd, 1, 1, arr) != 0) {
		printf("Error sending WebSocket frame\n");
	}
	 pthread_mutex_lock( &mutex1 );
        updateDetails(userdetail->ingame,userdetail->connfd);
        activeuserssend();
        pthread_mutex_unlock( &mutex1 );
}

void handleRequestGame(struct game_user_details* head,int userid){
    struct game_user_details* current = head;
    char arr[100];
    sprintf(arr,"gameRequest => %d",userid);
    while (current != NULL) {
        if(userid != current->userid && current->ingame == 0){
        	if (send_websocket_frame(current->connfd, 1, 1, arr) != 0) {
		    printf("Error sending WebSocket frame\n");
		}
        }
        current = current->next;
    }
}

void* handle_game_client() {
    
    struct game_user_details* user_detail = user_details;
    int connfd = user_detail->connfd;
    user_detail->userid = connfd;
    for(int i=0;i<3;i++){
    	for(int j=0;j<3;j++){
    		user_detail->board[i][j] = ' ';
    	}
    }
    
    pthread_mutex_unlock( &mutex1 );
    char arr[100];
    int flag = 0;
    
    sprintf(arr,"userid => %d",user_detail->userid);
    if (send_websocket_frame(connfd, 1, 1, arr) != 0) {
	   printf("Error sending WebSocket frame\n");
    }
    
    activeuserssend();
    
    while (1) {
        char received_data[1024];

        char *decoded_data = NULL;
        
        ssize_t bytes_received = recv(connfd, received_data, sizeof(received_data), 0);
        if (bytes_received == -1) {
            perror("Receive failed");
            close(connfd);
            continue;
        }
        
        if (process_websocket_frame(received_data, bytes_received, &decoded_data,connfd,&flag) != 0) {
            printf("Error processing WebSocket frame\n");
            close(connfd);
            continue;
        }
        
        if(flag == 1){
        	flag = 0;
        	continue;
        }
        

        if(decoded_data){
		printf("Received message from client: %s\n", decoded_data);
		
		
		char* ptr = NULL;
		if(ptr = strstr(decoded_data,"gameRequest")){
			ptr += 15;
			handleGameRequest(atoi(ptr),user_details,connfd);
			free(decoded_data);
			continue;
		}
		
		if(ptr = strstr(decoded_data,"requestGame")){
			handleRequestGame(user_details,connfd);
			free(decoded_data);
			continue;
		}
		
		if(ptr = strstr(decoded_data,"acceptGameRequest")){
			ptr += 21;
			handleacceptGame(atoi(ptr),user_details,connfd);
			free(decoded_data);
			continue;
		}
		
		if(ptr = strstr(decoded_data,"gameMove")){
			ptr += 12;
			handleGamemove(atoi(ptr),user_details,connfd);
			free(decoded_data);
			continue;
		}
		
		if(ptr = strstr(decoded_data,"endGame")){
			handleEndGame(user_detail,connfd);
			free(decoded_data);
			continue;
		}
		
		if(ptr = strstr(decoded_data,"activeUsers")){
		    if (send_websocket_frame(connfd, 1, 1, extractActiveUsersString(user_details,user_detail->userid)) != 0){
			   printf("Error sending WebSocket frame\n");
		    }
		    free(decoded_data);
		    continue;
		}

		if (send_websocket_frame(connfd, 1, 1, decoded_data) != 0) {
		    printf("Error sending WebSocket frame\n");
		}
		free(decoded_data);
	}
        
    }

   close(connfd);
}


int main(int argc, char* argv[]) {
	int sockfd,connfd; 
	sockfd = server_creation();
	pthread_t thread1;
	
	printf("Tic-tac-toe server: waiting for connections...\n");
	 
	while(1){ 
		connfd = connection_accepting(sockfd);
			
		if(connfd == -1){
			continue;
		}
		
		
		if (pthread_create(&thread1, NULL, handle_game_client,NULL) != 0) {
		    perror("pthread_create");
		    close(connfd);
		    continue;
		}
		
		if (pthread_detach(thread1) != 0) {
		    perror("pthread_detach");
		}
	}
	close(sockfd); 
	return 1;
}
