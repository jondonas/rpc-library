#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <arpa/inet.h>
#include <limits.h>
#include <iostream>
#include <string.h>
#include <map>
#include <vector>
#include "rpc.h"
#include "rpc_extra.h"
#include "rpc_errors.h"
#include "debug.h"

#define SERVER_PORT_KEY(ip, port) (ip + std::to_string(port))

/***
 * CLASSES/STRUCTS
 ***/

struct Server
{
    std::string ip;
    std::string port;
    Server(std::string ip, std::string port);
};

class Proc
{
    int current_server;
    public:
    std::vector<Server> servers;
    Server *get_next_available_server();
};


/***
 * CONSTANTS
 ***/

#define SERVER_PORT      0

#define TRUE             1
#define FALSE            0

std::map<std::string, Proc *> PROCS;
Server *last_used_server = NULL;

/***
 * METHODS
 ***/

Server::Server(std::string ip, std::string port) : ip(ip), port(port)
{
}

Server *Proc::get_next_available_server()
{
    if (servers.size() == 0) return NULL;
    if (current_server >= servers.size()) current_server = 0;
    Server *server = &(servers[current_server]);
    if (last_used_server != NULL && server->ip == last_used_server->ip)
    {
        current_server = (current_server + 1) % servers.size();
        server = &(servers[current_server]);
    }
    current_server = (current_server + 1) % servers.size();
    last_used_server = server;
    return server;
}

/***
 * BINDER
 ***/

// Maps a procedure name with a server
void register_proc_server(std::string proc_name, std::string server_ip, std::string server_port)
{
    Server server(server_ip, server_port);
    Proc *proc = NULL;
    try
    {
        proc = PROCS.at(proc_name);
    }
    catch (std::out_of_range e)
    {
        proc = new Proc();
        PROCS[proc_name] = proc;
    }

    // Avoid adding existing proc-server mapping
    bool found_proc_server = false;
    std::vector<Server> proc_servers = proc->servers;
    for (int i = 0; i < proc_servers.size(); ++i)
    {
        if (proc_servers[i].ip == server_ip && proc_servers[i].port == server_port)
        {
            found_proc_server = true;
            break;
        }
    }

    if (!found_proc_server)
    {
        proc->servers.push_back(server);
    }
}

// Removes a mapping between a procedure name and a server
void remove_proc_server(std::string server_ip, std::string server_port)
{
    int initial_procs_len = PROCS.size();
    for (std::map<std::string, Proc *>::iterator it = PROCS.begin(); it != PROCS.end(); ++it)
    {
        std::vector<Server> proc_servers = it->second->servers;
        for (int i = 0; i < proc_servers.size(); ++i)
        {
            if (proc_servers[i].ip == server_ip && proc_servers[i].port == server_port)
            {
                proc_servers.erase(proc_servers.begin() + i);
            }
        }

        it->second->servers = proc_servers;

        if (proc_servers.size() == 0)
        {
            PROCS.erase(it);
        }
    }  
    //DEBUG("PROCS: Initial size %d, final size %d\n", initial_procs_len, PROCS.size());
}

// Returns the next available server for a given procedure name, otherwise returns NULL.
Server *get_proc_server(std::string proc_name)
{
    Server *server = NULL;
    try
    {
       Proc *proc = PROCS.at(proc_name);
       server = proc->get_next_available_server();
    }
    catch (std::out_of_range e)
    {
    }
    return server;
}

int main(int argc, char *argv[])
{
    int i, len, rc, on = 1;
    int listen_sd, max_sd, new_sd;
    int desc_ready, end_server = FALSE;
    struct sockaddr_in addr;
    struct timeval timeout;
    fd_set master_set, working_set;
    char ip[INET_ADDRSTRLEN];
    uint16_t port;
    std::map<std::string, int> server_ports;

    listen_sd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sd < 0)
    {
        perror("socket() failed");
        exit(-1);
    }

    rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR, (char *)&on, sizeof(on));
    if (rc < 0)
    {
        perror("setsockopt() failed");
        close(listen_sd);
        exit(-1);
    }

    rc = ioctl(listen_sd, FIONBIO, (char *)&on);
    if (rc < 0)
    {
        perror("ioctl() failed");
        close(listen_sd);
        exit(-1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);
    rc = bind(listen_sd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0)
    {
        perror("bind() failed");
        close(listen_sd);
        exit(-1);
    }

    // Print binder details
    char hostname[HOST_NAME_MAX];
    gethostname(hostname, HOST_NAME_MAX);
    struct hostent *host;
    struct in_addr **addresses;
    host = gethostbyname(hostname);
    addresses = (struct in_addr **)host->h_addr_list;
    std::cout << "BINDER_ADDRESS " << inet_ntoa(*addresses[0]) << std::endl;

    struct sockaddr_in sin;
    socklen_t addrlen = sizeof(sin);
    getsockname(listen_sd, (struct sockaddr *)&sin, &addrlen);
    int binder_port = ntohs(sin.sin_port);
    std::cout << "BINDER_PORT " << binder_port << std::endl;

    // Set the listen back log
    rc = listen(listen_sd, 32);
    if (rc < 0)
    {
        perror("listen() failed");
        close(listen_sd);
        exit(-1);
    }

    // Initialize the master fd_set
    FD_ZERO(&master_set);
    max_sd = listen_sd;
    FD_SET(listen_sd, &master_set);

    do
    {
        memcpy(&working_set, &master_set, sizeof(master_set));
        rc = select(max_sd + 1, &working_set, NULL, NULL, NULL);

        if (rc < 0)
        {
            perror("  select() failed");
            break;
        }

        desc_ready = rc;
        for (i=0; i <= max_sd && desc_ready > 0; ++i)
        {
            if (FD_ISSET(i, &working_set))
            {
                desc_ready -= 1;

                if (i == listen_sd)
                {
                    do
                    {
                        new_sd = accept(listen_sd, NULL, NULL);
                        if (new_sd < 0)
                        {
                            if (errno != EWOULDBLOCK)
                            {
                                perror("  accept() failed");
                                end_server = TRUE;
                            }
                            break;
                        }

                        FD_SET(new_sd, &master_set);
                        if (new_sd > max_sd)
                            max_sd = new_sd;
                    } while (new_sd != -1);
                }
                else
                {
                    bool close_conn = false;
                    int type = -1;
                    rc = recv(i, &type, sizeof(type), 0);
                    if (rc < 0)
                    {
                        if (errno != EWOULDBLOCK)
                        {
                            perror("recv() failed");
                            close_conn = true;
                        }
                    }

                    if (rc == 0)
                    {
                        DEBUG("SERVER(%d): Connection closed\n", i);
                        close_conn = true;
                    }

                    if (close_conn) {
                        // Remove all proc mappings from server
                        struct sockaddr server_addr;
                        socklen_t server_addr_len = sizeof(server_addr);
                        int result = getpeername(i, &server_addr, &server_addr_len);
                        if (result == 0) {
                            struct sockaddr_in *server_addr_in = (struct sockaddr_in *)&server_addr;
                            std::string server_ip = inet_ntoa(server_addr_in->sin_addr);
                            int server_socket_port = ntohs(server_addr_in->sin_port);
                            try
                            {
                                std::string key = SERVER_PORT_KEY(server_ip, server_socket_port);
                                int server_port = server_ports.at(key);
                                DEBUG("SERVER(%d): Removing proc mappings, ip: %s, port: %d\n", i, server_ip.c_str(), server_port);
                                remove_proc_server(server_ip, std::to_string(server_port));
                                server_ports.erase(key);
                            }
                            catch (std::out_of_range e)
                            {
                            }
                        }

                        // Close socket connection
                        close(i);
                        FD_CLR(i, &master_set);
                        if (i == max_sd)
                        {
                            while (FD_ISSET(max_sd, &master_set) == FALSE)
                                max_sd -= 1;
                        }

                        continue;
                    }

                    len = rc;

                    // Determine message type
                    type = ntohl(type);
                    switch (type)
                    {
                        case REGISTER: 
                            {
                                char server_ip[ADDR_SIZE], proc_name[PROC_NAME_SIZE];
                                int arg_type, server_port;
                                std::string proc_signature;
                                recv(i, server_ip, sizeof(server_ip), 0);
                                recv(i, &server_port, sizeof(server_port), 0);
                                server_port = ntohl(server_port);
                                recv(i, proc_name, sizeof(proc_name), 0);
                                proc_signature = proc_name;
                                do
                                {
                                    recv(i, &arg_type, sizeof(arg_type), 0);
                                    arg_type = ntohl(arg_type);
                                    if (arg_type)
                                    {
                                        int array_len = arg_type & 0xFFFF;
                                        if (array_len > 0)
                                            proc_signature += std::to_string((arg_type & 0xFFFF0000) | 0x1);
                                        else
                                            proc_signature += std::to_string(arg_type);
                                    }
                                } while (arg_type != 0);

                                // Register server ip to <socket_port, client_port> map
                                struct sockaddr server_addr;
                                socklen_t server_addr_len = sizeof(server_addr);
                                int result = getpeername(i, &server_addr, &server_addr_len);
                                int msg;
                                if (result == 0)
                                {
                                    struct sockaddr_in *server_addr_in = (struct sockaddr_in *)&server_addr;
                                    int server_socket_port = ntohs(server_addr_in->sin_port);
                                    std::string key = SERVER_PORT_KEY(server_ip, server_socket_port);
                                    server_ports[key] = server_port;
                                    register_proc_server(proc_signature, server_ip, std::to_string(server_port));
                                    DEBUG("REGISTER(%d): proc: %s, ip: %s, port: %d\n", i, proc_signature.c_str(), server_ip, server_port);
                                    msg = htonl(REGISTER_SUCCESS);
                                    send(i, &msg, sizeof(msg), 0);
                                    msg = htonl(REGISTER_SUCCESS_NO_ERROR);
                                    send(i, &msg, sizeof(msg), 0);
                                }
                                else
                                {
                                    msg = htonl(REGISTER_FAILURE);
                                    send(i, &msg, sizeof(msg), 0);
                                    msg = htonl(REGISTER_FAILURE_ERROR_MAP);
                                    send(i, &msg, sizeof(msg), 0);
                                }
                            } break;
                        case LOC_REQUEST:
                            {
                                char proc_name[PROC_NAME_SIZE];
                                int arg_type;
                                std::string proc_signature;
                                recv(i, proc_name, sizeof(proc_name), 0);
                                proc_signature = proc_name;
                                do
                                {
                                    recv(i, &arg_type, sizeof(arg_type), 0);
                                    arg_type = ntohl(arg_type);
                                    if (arg_type)
                                    {
                                        int array_len = arg_type & 0xFFFF;
                                        if (array_len > 0)
                                            proc_signature += std::to_string((arg_type & 0xFFFF0000) | 0x1);
                                        else
                                            proc_signature += std::to_string(arg_type);
                                    }
                                } while (arg_type != 0);
                                Server *server = get_proc_server(proc_signature);
                                int msg;
                                if (server)
                                {
                                    msg = htonl(LOC_SUCCESS);
                                    send(i, &msg, sizeof(msg), 0);
                                    char msg_s[ADDR_SIZE];
                                    strcpy(msg_s, (server->ip).c_str());
                                    send(i, msg_s, sizeof(msg_s), 0);
                                    int port = htonl(std::stoi((server->port).c_str()));
                                    send(i, (char*)&port, sizeof(port), 0);
                                    DEBUG("LOC_REQUEST(%d): proc: %s, ip: %s, port: %s\n", i, proc_signature.c_str(), (server->ip).c_str(), (server->port).c_str());
                                }
                                else
                                {
                                    msg = htonl(LOC_FAILURE);
                                    send(i, &msg, sizeof(msg), 0);
                                    msg = htonl(LOC_FAILURE_ERROR_NO_SERVER);
                                    send(i, &msg, sizeof(msg), 0);
                                    DEBUG("LOC_REQUEST(%d) - FAIL: proc: %s\n", i, proc_signature.c_str());
                                }
                            } break;
                        case TERMINATE:
                            {
                                PROCS.clear();
                                for (i=0; i <= max_sd; ++i)
                                {
                                    if (FD_ISSET(i, &master_set))
                                    {
                                        struct sockaddr server_addr;
                                        socklen_t server_addr_len = sizeof(server_addr);
                                        int result = getpeername(i, &server_addr, &server_addr_len);
                                        if (result == 0) {
                                            struct sockaddr_in *server_addr_in = (struct sockaddr_in *)&server_addr;
                                            std::string server_ip = inet_ntoa(server_addr_in->sin_addr);
                                            int server_socket_port = ntohs(server_addr_in->sin_port);
                                            try
                                            {
                                                std::string key = SERVER_PORT_KEY(server_ip, server_socket_port);
                                                int server_port = server_ports.at(key);
                                                DEBUG("TERMINATE(%d): Terminating, ip: %s, port: %d\n", i, server_ip.c_str(), server_port);
                                                int msg = htonl(TERMINATE);
                                                send(i, &msg, sizeof(msg), 0);
                                            }
                                            catch (std::out_of_range e)
                                            {
                                            }
                                        }
                                    }
                                }
                                end_server = TRUE;
                            } break;
                        default:
                            break;
                    }
                }
            }
        }
    } while (end_server == FALSE);

    DEBUG("TERMINATING SERVER\n");
    for (i=0; i <= max_sd; ++i)
    {
        if (FD_ISSET(i, &master_set))
            close(i);
    }
}
