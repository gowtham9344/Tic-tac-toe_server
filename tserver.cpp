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
#include <signal.h>
#include <thread>
#include <mutex>

#define SA struct sockaddr 
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define PORT "8000"
#define BACKLOG 100

using namespace std;

class TcpServer
{   
    int socketfd;

    public:
        TcpServer(){
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
           return write(client_socket,buff,len);
        }

        int getResponse(int client_socket,uint8_t * buff,int len){
            return read(client_socket,buff,len);
        }

        int sendRequest(int client_socket,char * buff,int len){
           return write(client_socket,buff,len);
        }

        int getResponse(int client_socket,char * buff,int len){
            return read(client_socket,buff,len);
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
            if((rv = getaddrinfo(NULL,PORT,&hints,&servinfo)) != 0){
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
            if(listen(socketfd,BACKLOG) == -1){ 
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
    public:
    TcpServer tcp;

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

        char upgrade_response_format[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n";
        char response[2048];
        int len = sprintf(response, upgrade_response_format, accept_key);
        response[len] = '\0';
        len = tcp.sendRequest(client_socket, response, strlen(response));
        cout<<"length:"<<len<<endl;
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


    int processWebsocketFrame(uint8_t *data, size_t length, char **decodedData,int client_socket) {
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
        
        printf("opcode:%x\n",opcode);
        if (opcode == 0x9) {
            handlePing(data,length,client_socket);
            *decodedData = NULL;
            return 1; // return code for ping request
        } else if (opcode == 0x8) {
            cout<<"closes the connection"<<endl;
            return 2; // return code for close connection
        }
        printf("opcode:%x\n",opcode);

        *decodedData = (char *)malloc(payload_length + 1);

        
        if(mask)
            for (size_t i = 0; i < payload_length; ++i) {
            (*decodedData)[i] = data[payload_offset + i] ^ masking_key[i % 4];
        }

        (*decodedData)[payload_length] = '\0';
        //printf("#message length:%d\n",strlen(*decodedData));
        return 0; // return code for normal data
    }

    public:
    int webSocketCreate(){
        char buffer[2048];
        int client_socket = tcp.connection_accepting();
        int len = tcp.getResponse(client_socket,buffer,2048);
        buffer[len] = '\0';
        printf("buffer:%s\n",buffer);
        
        handleWebsocketUpgrade(client_socket,buffer);
        return client_socket;
    }

    void sendCloseFrame(int client_socket) {
        uint8_t close_frame[] = {0x88, 0x00};
        tcp.sendRequest(client_socket, close_frame, sizeof(close_frame));
        cout<<"close frame sent to the client"<<endl;
    }

    int sendWebsocketFrame(int client_socket, uint8_t fin, uint8_t opcode, char *payload) {

        uint8_t encoded_data[1024];

        int encoded_size = encodeWebsocketFrameHeader(fin, opcode, 0, strlen(payload), ( uint8_t *)payload, encoded_data);

        ssize_t bytes_sent = tcp.sendRequest(client_socket, encoded_data, encoded_size);
        cout<<"bytes_send"<<bytes_sent<<endl;
        if (bytes_sent == -1) {
            perror("Send failed");
            return 0;
        }

        cout<<"Message sent to client"<<endl;
        return 0;
    }

    int recvWebSocketFrame(char **decodedData,int client_socket){
        uint8_t data[2048]; 
        size_t length = 2048;
        int len = 0;
        if((len = tcp.getResponse(client_socket,data,length)) == -1){
            return -1; 
        }

        return processWebsocketFrame(data,length,decodedData,client_socket);
    }
};

struct gameUserDetails{
	int userid;
	int connfd;
	int inGameWith;
	char moveName;
	char board[3][3];
	struct gameUserDetails* next;

    gameUserDetails(int userid,int connfd,int inGameWith){
        this->userid = userid;
        this->connfd = connfd;
        this->inGameWith = inGameWith;
        for(int i=0;i<3;i++){
            for(int j=0;j<3;j++){
                this->board[i][j] = ' ';
            }
        }
        this->next = NULL;
    }
};

class TicTacToeServer{
    public:
        struct gameUserDetails *userDetails = NULL;
        WebSocketServer websocket = WebSocketServer();
        mutex mtx;

    void startServer(){
        cout<<"Tic-tac-toe server: waiting for connections..."<<endl;
        while(1){ 
            cout<<"client"<<endl;
            int connfd = addClient();
            printf("connfd:%d\n",connfd);
                
            if(connfd == -1){
                continue;
            }

            mtx.lock();
            thread myThread(&TicTacToeServer::handleGameClient,this,connfd);
            myThread.detach();
            cout<<"client1"<<endl;
        }
    }

    int addClient(){
        int connfd = websocket.webSocketCreate();
        if(connfd == -1){
            return -1;
        }
        struct gameUserDetails* userDetail = new gameUserDetails(connfd,connfd,0);
        userDetail->next = userDetails;
        userDetails = userDetail;
        return connfd;
    }

    char* extractActiveUsersString(int userid) {
        int length = 17;
        struct gameUserDetails* current = userDetails;
        if (current == NULL) {
            return strdup("activeUsers  => ");
        }
        

        while (current != NULL) {
            cout<<"hello\n";
            if(userid != current->userid && !current->inGameWith){
                length += 6;
            }
            current = current->next;
        }
        
        char* result = (char*)malloc(length + 10);
        if (result == NULL) {
            fprintf(stderr, "Memory allocation failed.\n");
            exit(EXIT_FAILURE);
        }

        current = userDetails;
        char* pos = result;
        pos += snprintf(pos,length,"activeUsers => ");
        while (current != NULL) {
        
            if(userid != current->userid && !(current->inGameWith)){
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
        struct gameUserDetails* current = userDetails;
        printf("hello\n");
        while(current!=NULL){
            
            if (websocket.sendWebsocketFrame(current->connfd, 1, 1, extractActiveUsersString(current->userid)) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            
            current = current->next;
        }
        
        display_details();
        printf("hello\n");
    }

    void updateDetails(int userid1,int userid2){
        struct gameUserDetails* current = userDetails;
        
        while(current != NULL){
            if(userid1 == current->userid){
                for(int i=0;i<3;i++){
                        for(int j=0;j<3;j++){
                            current->board[i][j] = ' ';
                        }
                    }
                    
                    current->inGameWith = 0;
            }
            if(userid2 == current->userid){
                for(int i=0;i<3;i++){
                        for(int j=0;j<3;j++){
                            current->board[i][j] = ' ';
                        }
                }
                current->inGameWith = 0;
            }
            current = current->next;
        }
    }

    void handleClose(int connfd) {
        struct gameUserDetails* current = userDetails;
        struct gameUserDetails* prev = NULL;

        while (current != NULL) {
            if(connfd == current->userid){
                if(current->inGameWith != 0){
                        char arr[100];
                        sprintf(arr,"gameOver => %d",current->inGameWith);
                        if (websocket.sendWebsocketFrame(current->inGameWith,1,1,arr) != 0) {
                            cout<<"Error sending WebSocket frame"<<endl;
                        }
                        updateDetails(current->userid,current->inGameWith);
                }
                break;
            }
            prev = current;
            current = current->next;
        }
        
        if(prev == NULL){
            userDetails = userDetails->next;
        }
        else{
            prev->next = prev->next->next;
        }
        activeuserssend();
        close(connfd);
        pthread_exit(NULL);
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

    void updateboard(int userid1,int userid2,int i,int j,char movename){
        struct gameUserDetails* current = userDetails;
        
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

    void handleGameRequest(int userid,int RequestUserid){
        struct gameUserDetails* current = userDetails;
        char arr[100];
        sprintf(arr,"gameRequest => %d",RequestUserid);
        while (current != NULL) {
            if(userid == current->userid){
                if (websocket.sendWebsocketFrame(current->connfd, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
            }
            current = current->next;
        }
        
    }

    void handleGameMove(int move,int senderUserid){

        struct gameUserDetails* current = userDetails;
        char arr[100];
        int i = move/3,j = move%3;
        
        while (current != NULL) {
            
            if(senderUserid == current->userid){
                    updateboard(senderUserid,current->inGameWith,i,j,current->moveName);
                if(checkWin(current->board)){
                    sprintf(arr,"gameMove => %d",move);
                    if (websocket.sendWebsocketFrame(current->inGameWith, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    sprintf(arr,"gameOver => %d",senderUserid);
                    if (websocket.sendWebsocketFrame(current->inGameWith, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    if (websocket.sendWebsocketFrame(current->connfd, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    updateDetails(senderUserid,current->inGameWith);
                    activeuserssend();
                    break;
                }
                if(checkDraw(current->board)){
                    sprintf(arr,"gameMove => %d",move);
                    if (websocket.sendWebsocketFrame(current->inGameWith, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    sprintf(arr,"gameOver => %d",0);
                    if (websocket.sendWebsocketFrame(current->inGameWith, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    if (websocket.sendWebsocketFrame(current->connfd, 1, 1, arr) != 0) {
                        cout<<"Error sending WebSocket frame"<<endl;
                    }
                    
                    updateDetails(senderUserid,current->inGameWith);
                    activeuserssend();
                    break;
                }
                sprintf(arr,"gameMove => %d",move);
                        
                if (websocket.sendWebsocketFrame(current->inGameWith, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
                
                sprintf(arr,"yourTurn");
                if(websocket.sendWebsocketFrame(current->inGameWith,1,1, arr) != 0){
                    cout<<"Error sending WebSocket frame"<<endl;
                }
                
                sprintf(arr,"OpponentTurn");
                if(websocket.sendWebsocketFrame(current->connfd,1,1, arr) != 0){
                    cout<<"Error sending WebSocket frame"<<endl;
                }
                break;
            }
            current = current->next;
        }
        
    }


    void handleEndGame(struct gameUserDetails* userDetail,int userid){
        char arr[100];		
        sprintf(arr,"gameOver => %d",userDetail->inGameWith);
        if (websocket.sendWebsocketFrame(userDetail->inGameWith, 1, 1, arr) != 0) {
            cout<<"Error sending WebSocket frame"<<endl;
        }
        if (websocket.sendWebsocketFrame(userDetail->connfd, 1, 1, arr) != 0) {
            cout<<"Error sending WebSocket frame"<<endl;
        }
        updateDetails(userDetail->inGameWith,userDetail->connfd);
        activeuserssend();
    }

    void handleRequestGame(int userid){
        struct gameUserDetails* current = userDetails;
        char arr[100];
        sprintf(arr,"gameRequest => %d",userid);
        while (current != NULL) {
            if(userid != current->userid && current->inGameWith == 0){
                if (websocket.sendWebsocketFrame(current->connfd, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
            }
            current = current->next;
        }
    }

    void handleAcceptGame(int userid,int acceptUserid){
        struct gameUserDetails* current = userDetails;
        char arr[100];
        sprintf(arr,"gameStart => %d, o",acceptUserid);
        int flag = 0;
        struct gameUserDetails* accepter = NULL;

        while (current != NULL) {
            if(userid == current->userid && current->inGameWith == 0){
                if (websocket.sendWebsocketFrame(current->connfd, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
                current->inGameWith = acceptUserid;
                current->moveName = 'o';
                flag = 1;
            
                sprintf(arr,"OpponentTurn");
                if(websocket.sendWebsocketFrame(current->connfd,1,1, arr) != 0){
                    cout<<"Error sending WebSocket frame"<<endl;
                }
            }
            if(acceptUserid == current->userid){
                accepter = current;
            }
            current = current->next;
        }
        
        if(flag){
            sprintf(arr,"gameStart => %d, x",userid);
            accepter->inGameWith = userid;
            accepter->moveName = 'x';
            if (websocket.sendWebsocketFrame(acceptUserid, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            sprintf(arr,"yourTurn");
            if(websocket.sendWebsocketFrame(acceptUserid,1,1, arr) != 0){
               cout<<"Error sending WebSocket frame"<<endl;
            }
        }
        activeuserssend();
    }

    void display_details(){
        struct gameUserDetails* current = userDetails;

        printf("\n\ncurrent user details\n\n");
        while(current!=NULL){
            printf("$%d\n",current->connfd);
            current = current->next;
        }
    }

    void handleGameClient(int connfd) {
        struct gameUserDetails* userDetail = userDetails;
        char arr[100];

        sprintf(arr, "userid => %d", userDetail->userid);
        if (websocket.sendWebsocketFrame(userDetail->connfd, 1, 1, arr) != 0) {
            cout << "Error sending WebSocket frame" << endl;
        }
        mtx.unlock();
        activeuserssend();
        

            while (1) {
                char* decodedData = NULL;
                int flag = 0;

                if ((flag = websocket.recvWebSocketFrame(&decodedData, userDetail->connfd)) == -1) {
                    handleClose(userDetail->connfd);
                    continue;
                }

                // ping frame
                if (flag == 1) {
                    continue;
                }

                // close frame
                if (flag == 2) {
                    websocket.sendCloseFrame(userDetail->connfd);
                    mtx.lock();
                    handleClose(userDetail->connfd);
                    mtx.unlock();
                    break;
                }

                if (decodedData) {
                    cout << "Received message from client: " << decodedData << endl;

                    char* ptr = NULL;

                    if (ptr = strstr(decodedData, "gameRequest")) {
                        ptr += 15;
                        mtx.lock();
                        handleGameRequest(atoi(ptr), userDetail->connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "requestGame")) {
                        mtx.lock();
                        handleRequestGame(userDetail->connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "acceptGameRequest")) {
                        ptr += 21;
                        mtx.lock();
                        handleAcceptGame(atoi(ptr), userDetail->connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "gameMove")) {
                        ptr += 12;
                        mtx.lock();
                        handleGameMove(atoi(ptr), userDetail->connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "endGame")) {
                        mtx.lock();
                        handleEndGame(userDetail, userDetail->connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "activeUsers")) {
                        if (websocket.sendWebsocketFrame(userDetail->connfd, 1, 1, extractActiveUsersString(userDetail->userid)) != 0) {
                            cout << "Error sending WebSocket frame" << endl;
                        }
                        free(decodedData);
                        continue;
                    }

                    if(strlen(decodedData) == 0){
                        mtx.lock();
                        handleClose(userDetail->connfd);
                        mtx.unlock();
                        break;
                    }


                    if (!strlen(decodedData))
                        free(decodedData);
                }
        }
        close(userDetail->connfd);
        free(userDetail);
    }


};

int main(){
    signal(SIGPIPE,SIG_IGN);
    TicTacToeServer tttserver;
    tttserver.startServer();
    return 0;
}