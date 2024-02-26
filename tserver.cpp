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
#include <map>

#define SA struct sockaddr 
#define MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define PORT "8000"
#define BACKLOG 100

using namespace std;


// tcp server class used to create a server and accept clients and give the functionality to send and receive messages.
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


	// send request in the uint8_t* form to the client connected to the client_socket with the length len
        int sendRequest(int client_socket,uint8_t * buff,int len){
           return write(client_socket,buff,len);
        }

	// receive request in the uint8_t* form from the client connected to the client_socket with the maximum length len
        int getResponse(int client_socket,uint8_t * buff,int len){
            return read(client_socket,buff,len);
        }

	// send request in the char* form to the client connected to the client_socket with the length len
        int sendRequest(int client_socket,char * buff,int len){
           return write(client_socket,buff,len);
        }

	// receive request in the char* form from the client connected to the client_socket with the maximum length len
        int getResponse(int client_socket,char * buff,int len){
            return read(client_socket,buff,len);
        }

        

    private:
    
    	// create a server in the port mentioned in the MACRO PORT
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
        
        
	// return the ip address of the given sockaddr structure
        void *get_in_addr(struct sockaddr *sa){
            if(sa->sa_family == AF_INET){
                return &(((struct sockaddr_in*)sa)->sin_addr);	
            }
            return &(((struct sockaddr_in6*)sa)->sin6_addr);
        }
};



// websocketserver gives the functionality of the tcp to websocket upgrade and frame encoding and decoding of messages send and receive using websocket 
class WebSocketServer{
    public:
    TcpServer tcp;
    char upgrade_response_format[200] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n";

    private:
    
    //generate a random mask for encoding the frame
    void generateRandomMask(uint8_t *mask) {
        srand(time(NULL));

        for (size_t i = 0; i < 4; ++i) {
            mask[i] = rand() & 0xFF;
        }
    }
 
    //mask the payload using the mask values
    void maskPayload(uint8_t *payload, size_t payload_length, uint8_t *mask) {
        for (size_t i = 0; i < payload_length; ++i) {
            payload[i] ^= mask[i % 4];
        }
    }

   // encode the message in the websocket frame format
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

    // decode the websocket frame to get the message 
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


   // calculate the sha1 for given clientkey and encode to the base64 format to get acceptket to be send in the handshaking process of 
   // websocket connection
    void calculateWebSocketAccept(const char *clientKey, char *acceptKey) {
	char combinedKey[1024] = "";
	strcpy(combinedKey, clientKey);
	//cout<<"clientkey:"<<clientKey<<endl;
	strcat(combinedKey, MAGIC_STRING);
	//cout<<"combinedkey:"<<combinedKey<<endl;
	memset(acceptKey,'\0',50);
	unsigned char sha1Hash[SHA_DIGEST_LENGTH];
	SHA1(reinterpret_cast<const unsigned char*>(combinedKey), strlen(combinedKey), sha1Hash);

	BIO* b64 = BIO_new(BIO_f_base64());
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);

	BIO* bio = BIO_new(BIO_s_mem());
	BIO_push(b64, bio);

	BIO_write(b64, sha1Hash, SHA_DIGEST_LENGTH);
	BIO_flush(b64);

	BUF_MEM* bptr;
	BIO_get_mem_ptr(b64, &bptr);

	strcpy(acceptKey, bptr->data);

	size_t len = strlen(acceptKey);
	
	if (len > 0 && acceptKey[len - 1] == '\n') {
	acceptKey[len - 1] = '\0';
	}
	acceptKey[28] = '\0';
	//cout<<"acceptKey:"<<acceptKey<<endl;

	BIO_free_all(b64);
    }


   // receive upgrade message and send the correct message for the given upgrade request.
    void handleWebsocketUpgrade(int client_socket, char *request) {

           if (strstr(request, "Upgrade: websocket") == nullptr) {
		std::cerr << "Not a WebSocket upgrade request" << std::endl;
		return;
	    }

	    const char* key_start = strstr(request, "Sec-WebSocket-Key: ") + 19;
	    const char* key_end = strchr(key_start, '\r');

	    if (!key_start || !key_end) {
		std::cerr << "Invalid Sec-WebSocket-Key header" << std::endl;
		return;
	    }

	    char client_key[1024]="";
	    strncpy(client_key, key_start, key_end - key_start);
	    client_key[key_end - key_start] = '\0';

	    char accept_key[50]="";
	    calculateWebSocketAccept(client_key, accept_key);

	    char response[300]="";
	    int len = sprintf(response, upgrade_response_format, accept_key);
	    response[len] = '\0';
	    //cout<<"response:"<<response<<endl;
            len = tcp.sendRequest(client_socket, response, strlen(response));
            //cout<<"length:"<<len<<endl;
            cout<<"WebSocket handshake complete"<<endl;
    }


   // handle ping frame received from the client by sending pong frame 
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

   // get data and handle the messages based on the opcode recieved
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
        
        //printf("opcode:%x\n",opcode);
        if (opcode == 0x9) {
            handlePing(data,length,client_socket);
            *decodedData = NULL;
            return 1; // return code for ping request
        } else if (opcode == 0x8) {
            cout<<"closes the connection"<<endl;
            return 2; // return code for close connection
        }
        //printf("opcode:%x\n",opcode);

        *decodedData = (char *)malloc(payload_length + 1);

        
        if(mask)
            for (size_t i = 0; i < payload_length; ++i) {
            (*decodedData)[i] = data[payload_offset + i] ^ masking_key[i % 4];
        }

        (*decodedData)[payload_length] = '\0';
    
        return 0; // return code for normal data
    }

    public:
    // creation of websocket
    int webSocketCreate(){
        char buffer[2048];
        int client_socket = tcp.connection_accepting();
        int len = tcp.getResponse(client_socket,buffer,2048);
        buffer[len] = '\0';
        
        handleWebsocketUpgrade(client_socket,buffer);
        return client_socket;
    }

   // send close frame for client closing.
    void sendCloseFrame(int client_socket) {
        uint8_t close_frame[] = {0x88, 0x00};
        tcp.sendRequest(client_socket, close_frame, sizeof(close_frame));
        cout<<"close frame sent to the client"<<endl;
    }

    // send messages in the websocket frame format.
    int sendWebsocketFrame(int client_socket, uint8_t fin, uint8_t opcode, char *payload) {

        uint8_t encoded_data[1024];

        int encoded_size = encodeWebsocketFrameHeader(fin, opcode, 0, strlen(payload), ( uint8_t *)payload, encoded_data);

        ssize_t bytes_sent = tcp.sendRequest(client_socket, encoded_data, encoded_size);
    
        if (bytes_sent == -1) {
            perror("Send failed");
            return 0;
        }

        cout<<"Message sent to client"<<endl;
        return 0;
    }
 
    // receive messages in the websocket frame format.
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

// helps board with functionality of that board for tic_tac_toe server
class boardDetails{
    char board[3][3];

    public:
    boardDetails(){
        initializeBoard();
    }

    // check if the current board is tie or not
    int checkDraw()
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

    // check whether the board is in win or not
    int checkWin()
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


    // used to initialize the board with the ' ' 
    void initializeBoard(){

        for(int i=0;i<3;i++){

            for(int j=0;j<3;j++){

                board[i][j] = ' ';
            }
        }
    }

    // update the move in that board in position i,j
    void updateBoard(int i,int j,char move){
        board[i][j] = move;
    }
};

// structure for each client 
class gameUserDetails{
    public:
        int userid;
        int connfd;
        int inGameWith;
        char moveName;
        int boardNo;

    gameUserDetails(){
        this->userid = 0;
        this->connfd = 0;
        this->inGameWith = 0;
        this->boardNo = 0;
        this->moveName = ' ';
    }

    void updateUserid(int userid,int connfd){
        this->userid = userid;
        this->connfd = connfd;
    }

    void reinitialzeDetails(){
        inGameWith = 0;
        boardNo = 0;
        moveName = ' ';
    }

    void updateDetails(int inGameWith,int boardNo,char moveName){
        this->inGameWith = inGameWith;
        this->boardNo = boardNo;
        this->moveName = moveName;
    }

};


// gives the functionality for playing tic-tac-toe game
class TicTacToeServer{
        map<int,gameUserDetails> userDetails;
        map<int,boardDetails> boards;
        WebSocketServer websocket = WebSocketServer();
        mutex mtx;
        
    public:
    // start the server for tic-tac-toe game
    void startServer(){
        cout<<"Tic-tac-toe server: waiting for connections..."<<endl;
        while(1){ 

            int connfd = addClient();
                
            if(connfd == -1){
                continue;
            }

            mtx.lock();
            thread myThread(&TicTacToeServer::handleGameClient,this,connfd);
            myThread.detach();
        }
    }
    
    private:
    // add client if new client comes into the map
    int addClient(){
        int connfd = websocket.webSocketCreate();
        if(connfd == -1){
            return -1;
        }
        mtx.lock();
        if(userDetails.find(connfd) == userDetails.end())
            userDetails[connfd].updateUserid(connfd,connfd);
        mtx.unlock();

        return connfd;
    }

    // extract active users and give it in the string format
    char* extractActiveUsersString(int userid) {
        int length = 17;

        for(auto current:userDetails){
            if(userid != current.second.userid && !current.second.inGameWith){
                length += 6;
            }
        }
        
        char* result = (char*)malloc(length + 10);
        if (result == NULL) {
            fprintf(stderr, "Memory allocation failed.\n");
            exit(EXIT_FAILURE);
        }

        char* pos = result;
        pos += snprintf(pos,length,"activeUsers => ");
       for(auto current:userDetails){
            if(userid != current.second.userid && !current.second.inGameWith){
                pos += snprintf(pos, length + 1, "%d, ", current.second.userid);
            }
       }
        
        if (length == 17) {
            return strdup("activeUsers => ");
        }

        if (length > 17) {
            *(pos - 2) = '\0';
        }
    
        return result;
    }

    // send active users to all the clients
    void activeUsersSend(){
        for(auto current: userDetails){
            
            if (websocket.sendWebsocketFrame(current.second.connfd, 1, 1, extractActiveUsersString(current.second.userid)) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            
        }
    }

    // update details for given userid1 and userid2 and erase that board
    void updateDetails(int userid1,int userid2){
        gameUserDetails& user1 = userDetails[userid1];
        gameUserDetails& user2 = userDetails[userid2];
        boards.erase(user1.boardNo);
        user1.reinitialzeDetails();
        user2.reinitialzeDetails();
    }

    // remove client from the list and handling appropriately according to the in game with the client or not
    void handleClose(int connfd) {
        gameUserDetails& current = userDetails[connfd];
        if(current.inGameWith != 0){
            char arr[100];
            sprintf(arr,"gameOver => %d",current.inGameWith);
            if (websocket.sendWebsocketFrame(current.inGameWith,1,1,arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            updateDetails(current.userid,current.inGameWith);
        }
        
        userDetails.erase(connfd);
        activeUsersSend();
        close(connfd);
        mtx.unlock();
        pthread_exit(NULL);
    }


    // send request if someone request the game to the other
    void handleGameRequest(int userid,int RequestUserid){
        gameUserDetails& current = userDetails[userid];
        char arr[100];
        sprintf(arr,"gameRequest => %d",RequestUserid);
        if (websocket.sendWebsocketFrame(current.connfd, 1, 1, arr) != 0) {
            cout<<"Error sending WebSocket frame"<<endl;
        }
    }


    // handle each move and check whether the game ended or not
    void handleGameMove(int move,int senderUserid){
        char arr[100];
        int i = move/3,j = move%3;
        gameUserDetails& current = userDetails[senderUserid];
        boardDetails& board = boards[current.boardNo];
        board.updateBoard(i,j,current.moveName);
        if(board.checkWin()){
            sprintf(arr,"gameMove => %d",move);
            if (websocket.sendWebsocketFrame(current.inGameWith, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            sprintf(arr,"gameOver => %d",senderUserid);
            if (websocket.sendWebsocketFrame(current.inGameWith, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            if (websocket.sendWebsocketFrame(current.connfd, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            updateDetails(senderUserid,current.inGameWith);
            activeUsersSend();
        }
        else if(board.checkDraw()){
            sprintf(arr,"gameMove => %d",move);
            if (websocket.sendWebsocketFrame(current.inGameWith, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            sprintf(arr,"gameOver => %d",0);
            if (websocket.sendWebsocketFrame(current.inGameWith, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            if (websocket.sendWebsocketFrame(current.connfd, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            
            updateDetails(senderUserid,current.inGameWith);
            activeUsersSend();
        }
        else{
            sprintf(arr,"gameMove => %d",move);
                        
            if (websocket.sendWebsocketFrame(current.inGameWith, 1, 1, arr) != 0) {
                cout<<"Error sending WebSocket frame"<<endl;
            }
            
            sprintf(arr,"yourTurn");
            if(websocket.sendWebsocketFrame(current.inGameWith,1,1, arr) != 0){
                cout<<"Error sending WebSocket frame"<<endl;
            }
            
            sprintf(arr,"OpponentTurn");
            if(websocket.sendWebsocketFrame(current.connfd,1,1, arr) != 0){
                cout<<"Error sending WebSocket frame"<<endl;
            }
        }    
           
    }

   // handle end game if the game is ended
    void handleEndGame(int userid){
        char arr[100];		
        gameUserDetails& userDetail = userDetails[userid];
        sprintf(arr,"gameOver => %d",userDetail.inGameWith);
        if (websocket.sendWebsocketFrame(userDetail.connfd, 1, 1, arr) != 0) {
            cout<<"Error sending WebSocket frame"<<endl;
        }
        if (websocket.sendWebsocketFrame(userDetail.inGameWith, 1, 1, arr) != 0) {
            cout<<"Error sending WebSocket frame"<<endl;
        }
        updateDetails(userDetail.inGameWith,userDetail.connfd);
        activeUsersSend();
    }

    // send requestgame to all the userids 
    void handleRequestGame(int userid){
        char arr[100];
        sprintf(arr,"gameRequest => %d",userid);
        for(auto userDetail: userDetails){
            if(userid != userDetail.second.userid && userDetail.second.inGameWith == 0){
                if (websocket.sendWebsocketFrame(userDetail.second.connfd, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
            }
        }
    }


    // handle acceptgame message to start the game or not
    void handleAcceptGame(int userid,int acceptUserid){
        char arr[100];
        sprintf(arr,"gameStart => %d, o",acceptUserid);

        if (userDetails.find(userid) != userDetails.end()) {
            gameUserDetails& current = userDetails[userid];
            if(current.inGameWith == 0){
                gameUserDetails& accepter = userDetails[acceptUserid];

                if (websocket.sendWebsocketFrame(current.connfd, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }

                boards[current.userid].initializeBoard();
                current.updateDetails(acceptUserid,userid,'o');
            
                sprintf(arr,"OpponentTurn");
                if(websocket.sendWebsocketFrame(current.connfd,1,1, arr) != 0){
                    cout<<"Error sending WebSocket frame"<<endl;
                }

                sprintf(arr,"gameStart => %d, x",userid);
                accepter.updateDetails(userid,userid,'x');
                if (websocket.sendWebsocketFrame(acceptUserid, 1, 1, arr) != 0) {
                    cout<<"Error sending WebSocket frame"<<endl;
                }
                sprintf(arr,"yourTurn");
                if(websocket.sendWebsocketFrame(acceptUserid,1,1, arr) != 0){
                    cout<<"Error sending WebSocket frame"<<endl;
                }
            }
        }
        activeUsersSend();
    }


    // handle the client based request received
    void handleGameClient(int connfd) {
        gameUserDetails& userDetail = userDetails[connfd];
        char arr[100];

        sprintf(arr, "userid => %d", userDetail.userid);
        if (websocket.sendWebsocketFrame(userDetail.connfd, 1, 1, arr) != 0) {
            cout << "Error sending WebSocket frame" << endl;
        }
        mtx.unlock();
        activeUsersSend();
        

            while (1) {
                char* decodedData = NULL;
                int flag = 0;

                if ((flag = websocket.recvWebSocketFrame(&decodedData, userDetail.connfd)) == -1) {
                    mtx.lock();
                    handleClose(userDetail.connfd);
                    break;
                }

                // ping frame
                if (flag == 1) {
                    continue;
                }

                // close frame
                if (flag == 2) {
                    websocket.sendCloseFrame(userDetail.connfd);
                    mtx.lock();
                    handleClose(userDetail.connfd);
                    break;
                }

                if (decodedData) {
                    cout << "Received message from client: " << decodedData << endl;

                    char* ptr = NULL;

                    if (ptr = strstr(decodedData, "gameRequest")) {
                        ptr += 15;
                        mtx.lock();
                        handleGameRequest(atoi(ptr), userDetail.connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "requestGame")) {
                        mtx.lock();
                        handleRequestGame(userDetail.connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "acceptGameRequest")) {
                        ptr += 21;
                        mtx.lock();
                        handleAcceptGame(atoi(ptr), userDetail.connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "gameMove")) {
                        ptr += 12;
                        mtx.lock();
                        handleGameMove(atoi(ptr), userDetail.connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "endGame")) {
                        mtx.lock();
                        handleEndGame(userDetail.connfd);
                        free(decodedData);
                        mtx.unlock();
                        continue;
                    }

                    if (ptr = strstr(decodedData, "activeUsers")) {
                        if (websocket.sendWebsocketFrame(userDetail.connfd, 1, 1, extractActiveUsersString(userDetail.userid)) != 0) {
                            cout << "Error sending WebSocket frame" << endl;
                        }
                        free(decodedData);
                        continue;
                    }

                    if(strlen(decodedData) == 0){
                        mtx.lock();
                        handleClose(userDetail.connfd);
                        mtx.unlock();
                        break;
                    }


                    if (!strlen(decodedData))
                        free(decodedData);
                }
        }
        close(userDetail.connfd);
        userDetails.erase(userDetail.connfd);
    }


};

int main(){
    TicTacToeServer tttserver;
    tttserver.startServer();
    return 0;
}
