#include "rpc.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>

// Type values for messages
// This should be moved to a header file or something later
#define REGISTER			1
#define LOC_REQUEST			2
#define LOC_SUCCESS			3
#define LOC_FAILURE			4
#define EXECUTE				5
#define EXECUTE_SUCCESS	    6
#define	EXECUTE_FAILURE     7
#define TERMINATE			8

int rpcCall(char* name, int* argTypes, void** args) {
    // Get binder address from env variables
    int binder_port;
    char *binder_addr = getenv("BINDER_ADDRESS");
    char *binder_port_c = getenv("BINDER_PORT");
    if (!binder_addr || !binder_port_c) {
        // Missing an env variable
        return -1;
    }
    binder_port = atoi(binder_port_c);

    // Setup socket and connect to server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(binder_port);
    inet_pton(AF_INET, binder_addr, &server.sin_addr);
  
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
    	// Couldn't connect to server
    	return -2;
    }

    // Send LOC_REQUEST message
    int msg = htonl(LOC_REQUEST);
    send(sock, (char*)&msg, 4, 0);
    // Send function name. Name is 64 bits. One extra bit for null terminator
    // TODO: test that I'm sending this right
    char name_send[65];
    strncpy(name_send, name, 64);
    send(sock, name_send, 65, 0);
    // Send message
    int arg_count = 0;
    while (argTypes[arg_count] != 0) {
    	msg = htonl(argTypes[arg_count]);
    	send(sock, (char*)&msg, 4, 0);
    	++arg_count;
    }
    // Terminator
    send(sock, 0, 4, 0);

    // Receive response
    // IDK if we can assume byte order for sending??
    // I just encode and decode for now, but we can remove if it's safe
    int type;
    recv(sock, &type, 4, 0);
    type = ntohl(type);
    if (type == LOC_FAILURE) {
    	// Get error code and return
    	int err;
    	recv(sock, &err, 4, 0);
    	err = ntohl(err);
    	return err;
    }
    // Else we have success
    // This assumes port is an integer at the end of the message and address is null terminated
    char server_addr[64];
    recv(sock, server_addr, 64, 0);
    int server_port;
    recv(sock, &server_port, 4, 0);
    server_port = ntohl(server_port);

    // Send execute request
    // This iterates through argTypes and sends the actual values found
    // at the arg
    msg = htonl(EXECUTE);
    send(sock, (char*)&msg, 4, 0);
    send(sock, name_send, 65, 0);
    // Send argTypes
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        msg = htonl(argTypes[arg_count]);
        send(sock, (char*)&msg, 4, 0);
        ++arg_count;
    }
    // Terminator
    send(sock, 0, 4, 0);
    // Send args
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        bool input = argTypes[arg_count] & 0x80000000;
        bool output = argTypes[arg_count] & 0x40000000;

        // Only send args that are input to the server
        if (!input) {
            ++arg_count;
            continue;
        }

        type = argTypes[arg_count] & 0xFF0000;
        int array = argTypes[arg_count] & 0xFFFF;
        int len;
        /*
        if (type == ARG_CHAR)
            ...
            Insert sizes here
            ...
        */
        // Send elements of array
        if (array > 0) {
            for (int i = 0; i < array; ++i) {
                // Send here
                continue;
            }    
        }
        // Send scalar
        else {
            continue;
        }

        msg = htonl(argTypes[arg_count]);
        send(sock, (char*)&msg, 4, 0);
        ++arg_count;
    }


    return 0;
}
