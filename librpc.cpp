#include "rpc.h"
#include "rpc_extra.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <netdb.h>

int binderConnect(int *sock) {
    // Get binder address from env variables
    int binder_port;
    char *binder_addr = getenv("BINDER_ADDRESS");
    char *binder_port_c = getenv("BINDER_PORT");
    if (!binder_addr || !binder_port_c) {
        // Missing an env variable
        return -1;
    }
    binder_port = atoi(binder_port_c);

    // Setup socket and connect to binder
    *sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(binder_port);
    inet_pton(AF_INET, binder_addr, &server.sin_addr);
  
    if (connect(*sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        // Couldn't connect to binder
        return -2;
    }

    return 0;
}

int rpcCall(char* name, int* argTypes, void** args) {
    int sock;
    int ret = binderConnect(&sock);
    if (ret < 0)
        return ret;

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
    msg = 0;
    send(sock, (char*)&msg, 4, 0);

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
    char server_addr[65];
    recv(sock, server_addr, 65, 0);
    int server_port;
    recv(sock, &server_port, 4, 0);
    server_port = ntohl(server_port);
    close(sock);

    // Connect to server
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    inet_pton(AF_INET, server_addr, &server.sin_addr);
  
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        // Couldn't connect to server
        return -3;
    }

    // Send EXECUTE request
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
    msg = 0;
    send(sock, (char*)&msg, 4, 0);

    // Send args
    int output_len;
    int output_array_len;
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        bool input = argTypes[arg_count] & 0x80000000;
        bool output = argTypes[arg_count] & 0x40000000;

        type = argTypes[arg_count] & 0xFF0000;
        int array_len = argTypes[arg_count] & 0xFFFF;
        int len;
        // Get size of argument
        if (type == ARG_CHAR) len = sizeof(char);
        else if (type == ARG_SHORT) len = sizeof(short);
        else if (type == ARG_INT) len = sizeof(int);
        else if (type == ARG_LONG) len = sizeof(long);
        else if (type == ARG_DOUBLE) len = sizeof(double);
        else if (type == ARG_FLOAT) len = sizeof(float);

        // Determine output type to save code duplication later
        if (output) {
            output_len = len;
            output_array_len = array_len;
        }

        // Only send args that are input to the server
        if (!input) {
            ++arg_count;
            continue;
        }

        // Send elements of array
        if (array_len > 0) {
            send(sock, args[arg_count], array_len*len, 0);
        }
        // Send scalar
        else
            send(sock, args[arg_count], len, 0);

        ++arg_count;
    }

    // Receive execute response
    recv(sock, &type, 4, 0);
    type = ntohl(type);
    if (type == EXECUTE_FAILURE) {
        // Get error code and return
        int err;
        recv(sock, &err, 4, 0);
        err = ntohl(err);
        return err;
    }
    // Else we have success
    // Success message format is different than assignment spec:
    //   EXECUTE_SUCCESS, args
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        // Only check for output arg. Assignment spec says there is only one
        bool output = argTypes[arg_count] & 0x40000000;

        if (!output) {
            ++arg_count;
            continue;
        }

        if (output_array_len > 0) {
            for (int i = 0; i < output_array_len; ++i)
                recv(sock, args[arg_count], output_len*output_array_len, 0);
        }
        else
            recv(sock, args[arg_count], output_len, 0);

        break;
    }

    close(sock);

    return 0;
}

int rpcInit(void) {
    int ret = binderConnect(&sock_binder);
    if (ret < 0)
        return ret;

    // Create socket to listen for clients
    int on = 1;
    struct sockaddr_in address;
    sock_client = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock_client, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
    ioctl(sock_client, FIONBIO, (char *)&on);

    // Bind socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    bind(sock_client, (struct sockaddr *)&address, sizeof(address));
    
    // Get client port
    struct sockaddr_in sin;
    socklen_t addrlen = sizeof(sin);
    getsockname(sock_client, (struct sockaddr *)&sin, &addrlen);
    client_port = ntohs(sin.sin_port);

    // Get ip of server
    char hostname[HOST_NAME_MAX];
    gethostname(hostname, HOST_NAME_MAX);
    struct hostent *host;
    struct in_addr **addresses;
    host = gethostbyname(hostname);
    addresses = (struct in_addr **) host->h_addr_list;
    strncpy(server_ip, inet_ntoa(*addresses[0]), 64);

    return 0;
}

int rpcRegister(char *name, int *argTypes, skeleton f) {
    // Alloc memory for saving in database later
    char *name_save = new char[65];

    // Send REGISTER message
    int msg = htonl(REGISTER);
    send(sock_binder, (char*)&msg, 4, 0);
    // Send server ip
    send(sock_binder, server_ip, 65, 0);
    // Send server port
    msg = htonl(client_port);
    send(sock_binder, (char*)&msg, 4, 0);
    // Send function name
    strncpy(name_save, name, 64);
    send(sock_binder, name_save, 65, 0);
    // Send argTypes
    int arg_count = 0;
    while (argTypes[arg_count] != 0) {
        msg = htonl(argTypes[arg_count]);
        send(sock_binder, (char*)&msg, 4, 0);
        ++arg_count;
    }
    // Terminator
    msg = 0;
    send(sock_binder, (char*)&msg, 4, 0);

    // Receive response
    int type;
    recv(sock_binder, &type, 4, 0);
    type = ntohl(type);
    // Get return code
    int status;
    recv(sock_binder, &status, 4, 0);
    status = ntohl(status);
    if (type == LOC_FAILURE) {
        // Return error code 
        delete[] name_save;
        return status;
    }
    // Else we have success. Register function in local database
    // Alloc space to copy over deets
    int *argTypes_save = new int[arg_count];
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        argTypes_save[arg_count] = argTypes[arg_count];
        ++arg_count;
    }
    argTypes_save[arg_count] = 0;

    database.push_back(std::make_tuple(name_save, argTypes_save, f));

    // status will be >= 0, so return it to include warnings
    return status;
}