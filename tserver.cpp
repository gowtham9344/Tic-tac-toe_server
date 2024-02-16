#include <iostream>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <arpa/inet.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <errno.h>

#define SA struct sockaddr 
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

using namespace std;

class TcpServer
{   
    int socketfd;
    int backlog;
    char* port;

    public:
        TcpServer(int backlog,char* port){
            this->backlog = backlog;
            this->port = port;
            serverCreation();
        }

        int getServerSocket(){
            return socketfd;
        }

        //connection establishment with the client
        //return connection descriptor to the calling function
        int connection_accepting(){
            int connfd;
            struct sockaddr_storage their_addr;
            char s[INET6_ADDRSTRLEN];
            socklen_t sin_size;
            
            sin_size = sizeof(their_addr); 
            connfd=accept(socketfd,(SA*)&their_addr,&sin_size); 
            if(connfd == -1){ 
                perror("\naccept error\n");
                return -1;
            } 

            inet_ntop(their_addr.ss_family,get_in_addr((struct sockaddr *)&their_addr),s, sizeof(s));
            cout << "\nserver: got connection from " << s << endl;
            
            return connfd;
        }

        int sendRequest(int client_socket,uint8_t * buff,int len){
           return send(client_socket,buff,len,0);
        }

        int getResponse(int client_socket,uint8_t * buff,int len){
            return recv(client_socket,buff,len,0);
        }

        int sendRequest(int client_socket,char * buff,int len){
           return send(client_socket,buff,len,0);
        }

        int getResponse(int client_socket,char * buff,int len){
            return recv(client_socket,buff,len,0);
        }

        

    private:
        void serverCreation(){
            struct addrinfo hints,*servinfo,*p;
            int yes = 1;
            int rv;
            memset(&hints,0,sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_PASSIVE;// my ip
            
            // set the address of the server with the port info.
            if((rv = getaddrinfo(NULL,port,&hints,&servinfo)) != 0){
                fprintf(stderr, "getaddrinfo: %s\n",gai_strerror(rv));	
                return;
            }
            
            // loop through all the results and bind to the socket in the first we can
            for(p = servinfo; p!= NULL; p=p->ai_next){
                socketfd=socket(p->ai_family, p->ai_socktype, p->ai_protocol);
                if(socketfd==-1){ 
                    perror("server: socket\n"); 
                    continue; 
                } 
                
                // SO_REUSEADDR is used to reuse the same port even if it was already created by this.
                // this is needed when the program is closed due to some system errors then socket will be closed automaticlly after few
                // minutes in that case before the socket is closed if we rerun the program then we have use the already used port
                if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1){
                    perror("setsockopt");
                    exit(1);	
                }
                        
                // it will help us to bind to the port.
                if (bind(socketfd, p->ai_addr, p->ai_addrlen) == -1) {
                    close(socketfd);
                    perror("server: bind");
                    continue;
                }
                break;
            }
            
            // server will be listening with maximum simultaneos connections of BACKLOG
            if(listen(socketfd,backlog) == -1){ 
                perror("listen");
                exit(1); 
            } 
        }

        void *get_in_addr(struct sockaddr *sa){
            if(sa->sa_family == AF_INET){
                return &(((struct sockaddr_in*)sa)->sin_addr);	
            }
            return &(((struct sockaddr_in6*)sa)->sin6_addr);
        }
};


class WebSocketServer{
    TcpServer tcp = TcpServer(10,"8000");

    public:
    WebSocketServer(){
        
    }
    private:
    void generateRandomMask(uint8_t *mask) {
        srand(time(NULL));

        for (size_t i = 0; i < 4; ++i) {
            mask[i] = rand() & 0xFF;
        }
    }

    void maskPayload(uint8_t *payload, size_t payload_length, uint8_t *mask) {
        for (size_t i = 0; i < payload_length; ++i) {
            payload[i] ^= mask[i % 4];
        }
    }

    int encodeWebsocketFrameHeader(uint8_t fin,uint8_t opcode,uint8_t mask,uint64_t payload_length,uint8_t *payload,uint8_t *frame_buffer) {
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
            generateRandomMask(frame_buffer + header_size - 4);
            maskPayload(payload, payload_length, frame_buffer + header_size - 4);
        }

        memcpy(frame_buffer + header_size, payload, payload_length);

        return header_size + payload_length; 
    }

    int decodeWebsocketFrameHeader(uint8_t *frame_buffer,uint8_t *fin,uint8_t *opcode,
        uint8_t *mask,uint64_t *payload_length
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


    void calculateWebsocketAccept(char *client_key,char *accept_key) {
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

    void handleWebsocketUpgrade(int client_socket, char *request) {

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
        calculateWebsocketAccept(key_start, accept_key);

        char *upgrade_response_format =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n";

        char response[2048];
        sprintf(response, upgrade_response_format, accept_key);
        tcp.sendRequest(client_socket, response, strlen(response));

        cout<<"WebSocket handshake complete"<<endl;
    }

    void handlePing(const uint8_t *data, size_t length,int client_socket) {

        if (length >= 1 && data[0] == 0x9) {
            // Send a pong frame in response
            uint8_t pong_frame[2] = {0x8A, 0x00};  
            ssize_t bytes_sent = tcp.sendRequest(client_socket, pong_frame, sizeof(pong_frame));

            if (bytes_sent == -1) {
                perror("Send failed");
            } else {
                cout<<"Pong Frame sent to client"<<endl;
            }
        }
    }


    int processWebsocketFrame(uint8_t *data, size_t length, char **decoded_data,int client_socket) {
        uint8_t fin, opcode, mask;
        uint64_t payload_length;
        uint8_t* masking_key;

        int header_size = decodeWebsocketFrameHeader(data, &fin, &opcode, &mask, &payload_length);
        if (header_size == -1) {
            cout<<"Error decoding WebSocket frame header"<<endl;
            return -1; // return code for error
        }
        
        if(mask){
            masking_key = header_size + data;
            header_size += 2;
        }
        
        header_size += 2;
        
        size_t payload_offset = header_size;
        
        
        if (opcode == 0x9) {
            handlePing(data,length,client_socket);
            *decoded_data = NULL;
            return 1; // return code for ping request
        } else if (opcode == 0x8) {
            printf("closes the connection\n");
            return 2; // return code for close connection
        }

        *decoded_data = (char *)malloc(payload_length + 1);

        
        if(mask)
            for (size_t i = 0; i < payload_length; ++i) {
            (*decoded_data)[i] = data[payload_offset + i] ^ masking_key[i % 4];
        }

        (*decoded_data)[payload_length] = '\0';
        return 0; // return code for ping / normal data
    }

    public:
    int webSocketCreate(){
        char buffer[2048];
        int client_socket = tcp.connection_accepting();
        tcp.getResponse(client_socket,buffer,2048);
        handleWebsocketUpgrade(client_socket,buffer);
        return client_socket;
    }

    void sendCloseFrame(int client_socket) {
        uint8_t close_frame[] = {0x88, 0x00};
        tcp.sendRequest(client_socket, close_frame, sizeof(close_frame));
    }

    int sendWebsocketFrame(int client_socket, uint8_t fin, uint8_t opcode, char *payload) {

        uint8_t encoded_data[1024];
        int encoded_size = encodeWebsocketFrameHeader(fin, opcode, 0, strlen(payload), ( uint8_t *)payload, encoded_data);


        ssize_t bytes_sent = tcp.sendRequest(client_socket, encoded_data, encoded_size);
        if (bytes_sent == -1) {
            perror("Send failed");
            return -1;
        }

        cout<<"Message sent to client"<<endl;
        return bytes_sent;
    }

    int recvWebSocketFrame(char **decoded_data,int client_socket){
        uint8_t data[2048]; 
        size_t length = 2048;

        tcp.getResponse(client_socket,data,length);
        return processWebsocketFrame(data,length,decoded_data,client_socket);
    }

};

class TicTacToeServer{
    
};