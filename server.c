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
	char name[1000];
	char ipaddr[INET6_ADDRSTRLEN];
	int connfd;
	int ingame;
	struct game_user_details* next;
};

pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;

// give IPV4 or IPV6  based on the family set in the sa
void *get_in_addr(struct sockaddr *sa){
	if(sa->sa_family == AF_INET){
		return &(((struct sockaddr_in*)sa)->sin_addr);	
	}
	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}



void decode_websocket_frame(const char *data, size_t length, char *decoded_data) {
    if (length < 6) {
        // Exit the current thread
        printf("message cannot be less than 6 length\n");
        pthread_exit(NULL);
    }

     // Extract basic header information
    unsigned char fin = (data[0] & 0x80) >> 7;
    unsigned char opcode = data[0] & 0x0F;
    unsigned char mask = (data[1] & 0x80) >> 7;
    size_t payload_length = data[1] & 0x7F;
    
    if (payload_length > length - 6) {
        // Exit the current thread
        printf("payload length cannot be greater than length\n");
        pthread_exit(NULL);
    }

    // Extract masking key
    const char *masking_key = data + 2;

    // Start of the payload data
    size_t payload_offset = 6;

    // Decode payload data with masking key
    for (size_t i = 0; i < payload_length; ++i) {
        decoded_data[i] = data[payload_offset + i] ^ masking_key[i % 4];
    }

    // Null-terminate the decoded data
    decoded_data[payload_length] = '\0';
}

void calculate_websocket_accept(const char *client_key, char *accept_key) {
    char combined_key[1024];
    strcpy(combined_key, client_key);
    strcat(combined_key, MAGIC_STRING);

    unsigned char sha1_hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)combined_key, strlen(combined_key), sha1_hash);

    // Base64 encode the SHA-1 hash
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

    BIO *bio = BIO_new(BIO_s_mem());
    BIO_push(b64, bio);

    BIO_write(b64, sha1_hash, SHA_DIGEST_LENGTH);
    BIO_flush(b64);

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);

    strcpy(accept_key, bptr->data);

    // Remove trailing newline character
    size_t len = strlen(accept_key);
    if (len > 0 && accept_key[len - 1] == '\n') {
        accept_key[len - 1] = '\0';
    }

    BIO_free_all(b64);
}


void handle_websocket_upgrade(int client_socket, const char *request) {
    // Check if it's a WebSocket upgrade request
    if (strstr(request, "Upgrade: websocket") == NULL) {
        fprintf(stderr, "Not a WebSocket upgrade request\n");
        return;
    }
    // Extract the value of Sec-WebSocket-Key header
    char *key_start = strstr(request, "Sec-WebSocket-Key: ") + 19;
    char *key_end = strchr(key_start, '\r');
    
    if (!key_start || !key_end) {
        fprintf(stderr, "Invalid Sec-WebSocket-Key header\n");
        return;
    }
    *key_end = '\0';

    // Calculate Sec-WebSocket-Accept header
    char accept_key[1024];
    calculate_websocket_accept(key_start, accept_key);

    // Send WebSocket handshake response
    const char *upgrade_response_format =
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
int connection_accepting(int sockfd,struct game_user_details** user_details){
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
	//printing the client name
	inet_ntop(their_addr.ss_family,get_in_addr((struct sockaddr *)&their_addr),s, sizeof(s));
	printf("\nserver: got connection from %s\n", s);
	
	//read(connfd,user_detail->name,1000);
	// Handle WebSocket upgrade
        char buffer[2048];
        ssize_t len = recv(connfd, buffer, sizeof(buffer), 0);
        if (len > 0) {
            buffer[len] = '\0';
            handle_websocket_upgrade(connfd, buffer);
        }

        
	user_detail->connfd = connfd;
	strcpy(user_detail->ipaddr,s);
	user_detail->ingame = 0;
	user_detail->next = *user_details;
	*user_details = user_detail;
	
	return connfd;
}


void encode_websocket_frame(const char *data, char *encoded_data, size_t length) {
    // Set FIN and opcode (text frame)
    encoded_data[0] = 0x81;

    // Set payload length (assuming length <= 125 for simplicity)
    encoded_data[1] = length;

    // Copy the payload data
    memcpy(encoded_data + 2, data, length);
}

// Function to handle a single client connection in a thread
void* handle_game_client(void* user_details) {
   
    struct game_user_details* user_detail = (struct game_user_details*)user_details;
   // printf("%s\n",user_details->name);
    int connfd = user_detail->connfd;
    printf("%d\n",user_detail->connfd);
    printf("%s\n",user_detail->name);
    printf("%d\n",user_detail->ingame);
    
    // Buffer for received data
	char received_data[1024];

	// Buffer for decoded data
	char decoded_data[1024];

	// Receive WebSocket frame from the client
	ssize_t bytes_received = recv(connfd, received_data, sizeof(received_data), 0);
	if (bytes_received == -1) {
		perror("Receive failed");
		close(connfd);
		exit(0);
	}

	// Decode the WebSocket frame
	decode_websocket_frame(received_data, bytes_received, decoded_data);

	// Print the decoded data
	printf("Received message from client: %s\n", decoded_data);
	
	char encoded_data[1024];
	encode_websocket_frame("willcome", encoded_data, 8);

	// Send the encoded message back to the client
	ssize_t bytes_sent = send(connfd, encoded_data, 8 + 2, 0);
	if (bytes_sent == -1) {
		perror("Send failed");
	} else {
		printf("Message sent to client\n");
	}	
		
        close(connfd);
}





int main(int argc, char* argv[]) {
	//server creation .
	int sockfd,connfd; 
	sockfd = server_creation();
	pthread_t thread1;
	struct game_user_details* user_details;
	
	printf("Tic-tac-toe server: waiting for connections...\n");
	 
	while(1){ 
		connfd = connection_accepting(sockfd,&user_details);
			
		if(connfd == -1){
			continue;
		}
		
		
		if (pthread_create(&thread1, NULL, handle_game_client,(void*)user_details) != 0) {
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
