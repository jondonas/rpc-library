#include "rpc.h"
#include "rpc_extra.h"
#include "rpc_database.h"
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
#include <pthread.h>
#include <iostream>
#include "debug.h"

#include <errno.h>

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

int getSize(int type) {
    if (type == ARG_CHAR) return sizeof(char);
    else if (type == ARG_SHORT) return sizeof(short);
    else if (type == ARG_INT) return sizeof(int);
    else if (type == ARG_LONG) return sizeof(long);
    else if (type == ARG_DOUBLE) return sizeof(double);
    else if (type == ARG_FLOAT) return sizeof(float);
}

int rpcCall(char* name, int* argTypes, void** args) {
    DEBUG("rpcCall\n");
    int sock, arg_len;
    int ret = binderConnect(&sock);
    if (ret < 0)
        return ret;

    // Send LOC_REQUEST message
    int msg = htonl(LOC_REQUEST);
    send(sock, (char*)&msg, sizeof(msg), 0);
    // Send function name. Name is 64 bits. One extra bit for null terminator
    // TODO: test that I'm sending this right
    char name_send[PROC_NAME_SIZE];
    strncpy(name_send, name, PROC_NAME_SIZE-1);
    send(sock, name_send, PROC_NAME_SIZE, 0);
    // Send message
    int arg_count = 0;
    while (argTypes[arg_count] != 0) {
        msg = htonl(argTypes[arg_count]);
        send(sock, (char*)&msg, sizeof(msg), 0);
        ++arg_count;
    }
    // Terminator
    msg = 0;
    send(sock, (char*)&msg, sizeof(msg), 0);
    arg_len = arg_count;

    // Receive response
    // IDK if we can assume byte order for sending??
    // I just encode and decode for now, but we can remove if it's safe
    int type;
    recv(sock, &type, sizeof(type), 0);
    type = ntohl(type);
    if (type == LOC_FAILURE) {
        DEBUG("rpcCall LOC_FAILURE\n");
        // Get error code and return
        int err;
        recv(sock, &err, 4, 0);
        err = ntohl(err);
        return err;
    }
    // Else we have success
    // This assumes port is an integer at the end of the message and address is null terminated
    char server_addr[ADDR_SIZE];
    recv(sock, server_addr, ADDR_SIZE, 0);
    int server_port;
    recv(sock, &server_port, 4, 0);
    server_port = ntohl(server_port);
    close(sock);

    DEBUG("rpcCall SERVER_ADDRESS: %s, SERVER_PORT: %d\n", server_addr, server_port);

    // Connect to server
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    inet_pton(AF_INET, server_addr, &server.sin_addr);
  
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        // Couldn't connect to server
        return -3;
    }

    // Send EXECUTE request
    // This iterates through argTypes and sends the actual values found
    // at the arg
    msg = htonl(EXECUTE);
    send(sock, &msg, sizeof(msg), 0);
    send(sock, name_send, PROC_NAME_SIZE, 0);
    msg = htonl(arg_len);
    send (sock, &msg, sizeof(msg), 0);
    // Send argTypes
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        msg = htonl(argTypes[arg_count]);
        send(sock, &msg, sizeof(msg), 0);
        ++arg_count;
    }

    // Send args
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        bool input = (argTypes[arg_count] >> 31) & 0x1;
        // Only send args that are input to the server
        if (!input) {
            ++arg_count;
            continue;
        }

        type = (argTypes[arg_count] >> 16) & 0xFF;
        int array_len = argTypes[arg_count] & 0xFFFF;
        // Get size of argument
        int len = getSize(type);

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
    recv(sock, &type, sizeof(type), 0);
    type = ntohl(type);
    if (type == EXECUTE_FAILURE) {
        // Get error code and return
        int err;
        recv(sock, &err, 4, 0);
        err = ntohl(err);
        return err;
    }

    DEBUG("rpcCall EXECUTE_SUCCESS: %d\n", type);
    // Else we have success
    // Success message format is different than assignment spec:
    //   EXECUTE_SUCCESS, args
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        // Only check for output arg. Assignment spec says there is only one
        bool output = (argTypes[arg_count] >> 30) & 0x1;

        if (!output) {
            ++arg_count;
            continue;
        }

        type = (argTypes[arg_count] >> 16) & 0xFF;
        int array_len = argTypes[arg_count] & 0xFFFF;
        int len = getSize(type);

        if (array_len > 0) {
            recv(sock, args[arg_count], len*array_len, 0);
        }
        else
            recv(sock, args[arg_count], len, 0);

        ++arg_count;
    }

    close(sock);

    DEBUG("Done rpcCall\n");

    return 0;
}

int rpcTerminate(void) {
    int sock;
    int ret = binderConnect(&sock);
    if (ret < 0)
        return ret;

    // Send TERMINATE message
    int msg = htonl(TERMINATE);
    send(sock, (char*)&msg, sizeof(msg), 0);

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
    //setsockopt(sock_client, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
    //ioctl(sock_client, FIONBIO, (char *)&on);
    DEBUG("sock_client created: %d\n", sock_client);

    // Bind socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    ret = bind(sock_client, (struct sockaddr *)&address, sizeof(address));
    if (ret >= 0)
        DEBUG("sock_client bound\n");

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
    DEBUG("SERVER_ADDRESS: %s\n", server_ip);
    DEBUG("CLIENT_PORT: %d\n", client_port);
    return 0;
}

int rpcRegister(char *name, int *argTypes, skeleton f) {
    DEBUG("rpcRegister\n");
    // Alloc memory for saving in database later
    char *name_save = new char[PROC_NAME_SIZE];

    // Send REGISTER message
    int msg = htonl(REGISTER);
    send(sock_binder, &msg, sizeof(msg), 0);
    // Send server ip
    send(sock_binder, server_ip, ADDR_SIZE, 0);
    // Send server port
    msg = htonl(client_port);
    send(sock_binder, &msg, sizeof(msg), 0);
    // Send function name
    strncpy(name_save, name, PROC_NAME_SIZE-1);
    send(sock_binder, name_save, PROC_NAME_SIZE, 0);
    // Send argTypes
    int arg_count = 0;
    while (argTypes[arg_count] != 0) {
        msg = htonl(argTypes[arg_count]);
        send(sock_binder, &msg, sizeof(msg), 0);
        ++arg_count;
    }
    // Terminator
    msg = 0;
    send(sock_binder, (char*)&msg, sizeof(msg), 0);

    // Receive response
    int type;
    recv(sock_binder, &type, sizeof(type), 0);
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
    DEBUG("Registered: %s %p\n", name_save, f);

    // status will be >= 0, so return it to include warnings
    return status;
}

void *connection_handler(void *socket_desc) {
    int sock = *(int *)socket_desc;
    DEBUG("Socket: %d\n", sock);

    // Get function name
    char name[PROC_NAME_SIZE];
    recv(sock, name, PROC_NAME_SIZE, 0);
    DEBUG("Running: %s\n", name);

    // Get argTypes
    int arg_len;
    recv(sock, &arg_len, 4, 0);
    arg_len = ntohl(arg_len);
    int *argTypes = new int[arg_len];

    for (int i = 0; i < arg_len; ++i) {
        int type;
        recv(sock, &type, sizeof(type), 0);
        argTypes[i] = ntohl(type);
    }

    // Check if function is registered
    skeleton skel;
    bool name_found = false, params_found = true;
    for (auto &entry: database) {
        // Check names match
        if (strcmp(std::get<0>(entry), name) == 0)
            name_found = true;
        // Check params match
        int arg_count = 0;
        while (argTypes[arg_count] != 0 && std::get<1>(entry)[arg_count] != 0) {
            int type1 = (argTypes[arg_count] >> 16) & 0xFF;
            int type2 = (std::get<1>(entry)[arg_count] >> 16) & 0xFF;
            if (type1 != type2) {
                params_found = false;
                break;
            }
            ++arg_count;
        }
        if (argTypes[arg_count] != 0 || std::get<1>(entry)[arg_count] != 0)
            params_found = false;

        if (name_found && params_found) {
            skel = std::get<2>(entry);
            break;
        }
        else {
            name_found = false;
            params_found = true;
        }
    }

    if (!(name_found && params_found)) {
        DEBUG("connection_handler NOT FOUND\n");
        return NULL;
    }

    DEBUG("Found: %s %p\n", name, skel);

    // Get args
    void **args = new void*[arg_len];
    for (int i = 0; i < arg_len; ++i) {
        int type = (argTypes[i] >> 16) & 0xFF;
        int array_len = argTypes[i] & 0xFFFF;
        int len = getSize(type);

        if (array_len == 0)
            array_len = 1;

        void *arg;
        arg = malloc(len*array_len);
        args[i] = arg;

        bool input = (argTypes[i] >> 31) & 0x1;
        // Only receive args that are input to the server
        if (!input) continue;

        recv(sock, args[i], len*array_len, 0);
    }

    DEBUG("connection_handler RUN SKELETON\n");

    // Run skeleton
    skel(argTypes, args);

    DEBUG("connection_handler SKELETON DONE\n");

    // Return execute success
    int msg = htonl(EXECUTE_SUCCESS);
    send(sock, (char*)&msg, sizeof(msg), 0);

    // Return result
    for (int i = 0; i < arg_len; ++i) {
        bool output = (argTypes[i] >> 30) & 0x1;

        if (!output) continue;

        int type = (argTypes[i] >> 16) & 0xFF;
        int array_len = argTypes[i] & 0xFFFF;
        int len = getSize(type);

        if (array_len == 0)
            array_len = 1;

        send(sock, args[i], array_len*len, 0);
        break;

    }

    // Free memory
    for (int i = 0; i < arg_len; ++i) free(args[i]);
    delete args;
    delete argTypes;

    DEBUG("connection_handler RESULT SENT\n");

    return 0;
}

int rpcExecute(void) {
    DEBUG("rpcExecute\n");
    // If no registered functions, return
    if (database.size() == 0)
        return -1;

    int client;
    listen(sock_client, 20);
    DEBUG("listening on sock_client: %d\n", sock_client);

    // Listen for incoming connections and spawn threads
    pthread_t t_connect;
    while (true) {
        client = accept(sock_client, NULL, NULL);
        if (client < 0) {
            DEBUG("accept error: %s\n", strerror(errno));
            break;
        }
        //DEBUG("client accepted: %d\n", client);
        // Receive message type
        int type;
        recv(client, &type, sizeof(type), 0);
        type = ntohl(type);

        if (type == EXECUTE) {
            // Serve the request
            DEBUG("rpcExecute EXECUTE\n");
            pthread_create(&t_connect, NULL, &connection_handler, (void *)&client);
        }
        else if (type == TERMINATE) {
            // Terminate the server. This cuts-off all currently running threads
            break;
        }
    }
     
    return 0;
}
