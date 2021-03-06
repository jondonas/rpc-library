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
#include <string>

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
    // Get size (int bytes) of primitive type
    if (type == ARG_CHAR) return sizeof(char);
    else if (type == ARG_SHORT) return sizeof(short);
    else if (type == ARG_INT) return sizeof(int);
    else if (type == ARG_LONG) return sizeof(long);
    else if (type == ARG_DOUBLE) return sizeof(double);
    else if (type == ARG_FLOAT) return sizeof(float);
}

int sendServer(char* name_send, int* argTypes, void** args, const char* server_addr, int server_port) {
    // Get arg_len
    int arg_count = 0, type;
    while (argTypes[arg_count] != 0) ++arg_count;
    int arg_len = arg_count;

    // Connect to server
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    inet_pton(AF_INET, server_addr, &server.sin_addr);
  
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0) {
        // Couldn't connect to server
        return -3;
    }

    // Send EXECUTE request
    // This iterates through argTypes and sends the actual values found
    // at the arg
    int msg = htonl(EXECUTE);
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

    // Else we have success
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        // Only check for output args
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

    return 0;
}

int rpcCall(char* name, int* argTypes, void** args) {
    int sock, arg_len;
    int ret = binderConnect(&sock);
    if (ret < 0)
        return ret;

    // Send LOC_REQUEST message
    int msg = htonl(LOC_REQUEST);
    send(sock, (char*)&msg, sizeof(msg), 0);
    // Send function name
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
    int type;
    recv(sock, &type, sizeof(type), 0);
    type = ntohl(type);
    if (type == LOC_FAILURE) {
        // Get error code and return
        int err;
        recv(sock, &err, 4, 0);
        err = ntohl(err);
        return err;
    }
    // Else we have success
    char server_addr[ADDR_SIZE];
    recv(sock, server_addr, ADDR_SIZE, 0);
    int server_port;
    recv(sock, &server_port, 4, 0);
    server_port = ntohl(server_port);
    close(sock);

    return sendServer(name_send, argTypes, args, server_addr, server_port);
}

int findEntryCache(char *name, int *argTypes) {
    // Returns the index of duplicate entry. -1 otherwise
    bool name_found = false, params_found = true;
    for (int i = 0; i < cached_procs.size(); ++i) {
        if (strcmp(std::get<0>(cached_procs[i]), name) == 0)
            name_found = true;
        // Check params match
        int arg_count = 0;
        while (argTypes[arg_count] != 0 && std::get<1>(cached_procs[i])[arg_count] != 0) {
            int type1 = (argTypes[arg_count] >> 16) & 0xFF;
            int type2 = (std::get<1>(cached_procs[i])[arg_count] >> 16) & 0xFF;

            if (type1 != type2) {
                params_found = false;
                break;
            }
            ++arg_count;
        }
        if (argTypes[arg_count] != 0 || std::get<1>(cached_procs[i])[arg_count] != 0)
            params_found = false;

        if (name_found && params_found)
            return i;
        else {
            name_found = false;
            params_found = true;
        }
    }

    return -1;
}

int rpcCacheCall(char* name, int* argTypes, void** args) {
    int sock, arg_len;
    int ret = binderConnect(&sock);
    if (ret < 0)
        return ret;

    char name_send[PROC_NAME_SIZE];
    strncpy(name_send, name, PROC_NAME_SIZE-1);

    // Loop and attempt to connect through cached servers
    int index = findEntryCache(name, argTypes);
    if (index >= 0) {
        for (auto &server: std::get<2>(cached_procs[index])) {
            ret = sendServer(name_send, argTypes, args, std::get<0>(server).c_str(), std::get<1>(server));
            if (ret >= 0)
                return ret;
        }
    }

    // Send CACHE_LOC_REQUEST message
    int msg = htonl(CACHE_LOC_REQUEST);
    send(sock, (char*)&msg, sizeof(msg), 0);
    // Send function name
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

    // Receive response
    int type;
    recv(sock, &type, sizeof(type), 0);
    type = ntohl(type);
    if (type == LOC_FAILURE) {
        // Get error code and return
        int err;
        recv(sock, &err, 4, 0);
        err = ntohl(err);
        return err;
    }
    // Else we have success
    int num;
    recv(sock, &num, sizeof(num), 0);
    num = ntohl(num);
    // Clear old proc from cached_procs first
    if (index >= 0)
        cached_procs.erase(cached_procs.begin() + index);

    std::vector<std::tuple<std::string, int>> r_servers;
    for (int i = 0; i < num; ++i) {
        char server_addr[ADDR_SIZE];
        recv(sock, server_addr, ADDR_SIZE, 0);
        int server_port;
        recv(sock, &server_port, 4, 0);
        server_port = ntohl(server_port);
        r_servers.push_back(std::make_tuple(std::string(server_addr), server_port));
    }
    // Alloc mem for name
    char *name_save = new char[PROC_NAME_SIZE];
    strncpy(name_save, name, PROC_NAME_SIZE-1);
    // Alloc mem for argTypes
    int *argTypes_save = new int[arg_len+1];
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        argTypes_save[arg_count] = argTypes[arg_count];
        ++arg_count;
    }
    argTypes_save[arg_count] = 0;
    cached_procs.push_back(std::make_tuple(name_save, argTypes_save, r_servers));

    close(sock);

    // Loop again and attempt to connect through cached servers
    for (auto &server: r_servers) {
        ret = sendServer(name_send, argTypes, args, std::get<0>(server).c_str(), std::get<1>(server));
        if (ret >= 0) {
            return ret;
        }
    }

    return -5;
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

    // Bind socket
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(0);
    ret = bind(sock_client, (struct sockaddr *)&address, sizeof(address));

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

int findEntryDB(char *name, int *argTypes) {
    // Returns the index of duplicate entry. -1 otherwise
    bool name_found = false, params_found = true;
    for (int i = 0; i < database.size(); ++i) {
        if (strcmp(std::get<0>(database[i]), name) == 0)
            name_found = true;
        // Check params match
        int arg_count = 0;
        while (argTypes[arg_count] != 0 && std::get<1>(database[i])[arg_count] != 0) {
            int type1 = (argTypes[arg_count] >> 16) & 0xFF;
            int type2 = (std::get<1>(database[i])[arg_count] >> 16) & 0xFF;

            if (type1 != type2) {
                params_found = false;
                break;
            }
            ++arg_count;
        }
        if (argTypes[arg_count] != 0 || std::get<1>(database[i])[arg_count] != 0)
            params_found = false;

        if (name_found && params_found)
            return i;
        else {
            name_found = false;
            params_found = true;
        }
    }

    return -1;
}

int rpcRegister(char *name, int *argTypes, skeleton f) {
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
    if (type == REGISTER_FAILURE) {
        // Return error code 
        delete[] name_save;
        return status;
    }
    // Else we have success. Register function in local database
    // Alloc space to copy over deets
    int *argTypes_save = new int[arg_count+1];
    arg_count = 0;
    while (argTypes[arg_count] != 0) {
        argTypes_save[arg_count] = argTypes[arg_count];
        ++arg_count;
    }
    argTypes_save[arg_count] = 0;

    // Delete any duplicate entry
    int index = findEntryDB(name_save, argTypes_save);
    if (index >= 0)
        database.erase(database.begin() + index);

    // Add new entry
    database.push_back(std::make_tuple(name_save, argTypes_save, f));

    // status will be >= 0, so return it to include warnings
    return status;
}

void sendExecFailure(int sock, int errcode) {
    int msg = htonl(EXECUTE_FAILURE);
    send(sock, (char*)&msg, sizeof(msg), 0);
    msg = htonl(errcode);
    send(sock, (char*)&msg, sizeof(msg), 0);
}

void *connection_handler(void *socket_desc) {
    int sock = *(int *)socket_desc;

    // Get function name
    char name[PROC_NAME_SIZE];
    recv(sock, name, PROC_NAME_SIZE, 0);

    // Get argTypes
    int arg_len;
    recv(sock, &arg_len, 4, 0);
    arg_len = ntohl(arg_len);
    int *argTypes = new int[arg_len+1];

    for (int i = 0; i < arg_len; ++i) {
        int type;
        recv(sock, &type, sizeof(type), 0);
        argTypes[i] = ntohl(type);
    }
    argTypes[arg_len] = 0;

    // Check if function is registered
    skeleton skel;
    int index = findEntryDB(name, argTypes);
    if (index >= 0)
        skel = std::get<2>(database[index]);
    else {
        delete argTypes;
        sendExecFailure(sock, -5);
        return NULL;
    }

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

    // Run skeleton
    int ret = skel(argTypes, args);

    if (ret < 0) {
        // Send EXECUTE FAILURE
        for (int i = 0; i < arg_len; ++i) free(args[i]);
        delete args;
        delete argTypes;
        sendExecFailure(sock, -6);
        return NULL;
    }

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

    }

    // Free memory
    for (int i = 0; i < arg_len; ++i) free(args[i]);
    delete args;
    delete argTypes;

    return 0;
}

void *binder_listen(void *) {
    while (true) {
        int type;
        recv(sock_binder, &type, sizeof(type), 0);
        type = ntohl(type);

        if (type == TERMINATE) {
            // Free memory
            for (auto &entry: database) {
                delete std::get<0>(entry);
                delete std::get<1>(entry);
            }
            for (auto &entry: cached_procs) {
                delete std::get<0>(entry);
                delete std::get<1>(entry);
            }
            // Terminate the server. This cuts-off all currently running threads
            shutdown(sock_client, SHUT_RDWR);
            running = false;
            break;
        }
    }
}

int rpcExecute(void) {
    // If no registered functions, return
    if (database.size() == 0)
        return -1;

    int client;
    listen(sock_client, 20);

    // Listen for TERMINATE from binder
    pthread_t t_binder;
    pthread_create(&t_binder, NULL, &binder_listen, NULL);

    // Listen for incoming connections and spawn threads
    pthread_t t_connect;
    while (true) {
        client = accept(sock_client, NULL, NULL);

        if (!running)
            break;

        // Receive message type
        int type;
        recv(client, &type, sizeof(type), 0);
        type = ntohl(type);

        if (type == EXECUTE) {
            // Serve the request
            pthread_create(&t_connect, NULL, &connection_handler, (void *)&client);
        }
    }
     
    return 0;
}
