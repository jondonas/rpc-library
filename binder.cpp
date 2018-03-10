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

std::map<std::string, Proc> PROCS;
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
  Proc proc = PROCS[proc_name];
  proc.servers.push_back(server);
}

// Removes a mapping between a procedure name and a server
void remove_proc_server(std::string server_ip)
{
  for (std::map<std::string, Proc>::iterator it = PROCS.begin(); it != PROCS.end(); ++it)
  {
    std::vector<Server> proc_servers = it->second.servers;
    for (int i = 0; i < proc_servers.size(); ++i)
    {
      if (proc_servers[i].ip == server_ip)
      {
        proc_servers.erase(proc_servers.begin() + i);
      }
    }
  }  
}

// Returns the next available server for a given procedure name, otherwise returns NULL.
Server *get_proc_server(std::string proc_name)
{
  return PROCS[proc_name].get_next_available_server();
}

int main()
{
  int    i, len, rc, on = 1;
  int    listen_sd, max_sd, new_sd;
  int    desc_ready, end_server = FALSE;
  struct sockaddr_in   addr;
  struct timeval       timeout;
  fd_set        master_set, working_set;
  char   ip[INET_ADDRSTRLEN];
  uint16_t port;

  /*************************************************************/
  /* Create an AF_INET stream socket to receive incoming       */
  /* connections on                                            */
  /*************************************************************/
  listen_sd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sd < 0)
  {
    perror("socket() failed");
    exit(-1);
  }

  /*************************************************************/
  /* Allow socket descriptor to be reuseable                   */
  /*************************************************************/
  rc = setsockopt(listen_sd, SOL_SOCKET,  SO_REUSEADDR,
                 (char *)&on, sizeof(on));
  if (rc < 0)
  {
    perror("setsockopt() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Set socket to be nonblocking. All of the sockets for    */
  /* the incoming connections will also be nonblocking since  */
  /* they will inherit that state from the listening socket.   */
  /*************************************************************/
  rc = ioctl(listen_sd, FIONBIO, (char *)&on);
  if (rc < 0)
  {
    perror("ioctl() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Bind the socket                                           */
  /*************************************************************/
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port        = htons(SERVER_PORT);
  rc = bind(listen_sd,
           (struct sockaddr *)&addr, sizeof(addr));
  if (rc < 0)
  {
    perror("bind() failed");
    close(listen_sd);
    exit(-1);
  }

  // Print binder info
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

  /*************************************************************/
  /* Set the listen back log                                   */
  /*************************************************************/
  rc = listen(listen_sd, 32);
  if (rc < 0)
  {
    perror("listen() failed");
    close(listen_sd);
    exit(-1);
  }

  /*************************************************************/
  /* Initialize the master fd_set                              */
  /*************************************************************/
  FD_ZERO(&master_set);
  max_sd = listen_sd;
  FD_SET(listen_sd, &master_set);

  /*************************************************************/
  /* Initialize the timeval struct to 3 minutes.  If no        */
  /* activity after 3 minutes this program will end.           */
  /*************************************************************/
  timeout.tv_sec  = 3 * 60;
  timeout.tv_usec = 0;

  /*************************************************************/
  /* Loop waiting for incoming connects or for incoming data   */
  /* on any of the connected sockets.                          */
  /*************************************************************/
  do
  {
    /**********************************************************/
    /* Copy the master fd_set over to the working fd_set.     */
    /**********************************************************/
    memcpy(&working_set, &master_set, sizeof(master_set));

    /**********************************************************/
    /* Call select() and wait 5 minutes for it to complete.   */
    /**********************************************************/
    printf("Waiting on select()...\n");
    rc = select(max_sd + 1, &working_set, NULL, NULL, &timeout);

    /**********************************************************/
    /* Check to see if the select call failed.                */
    /**********************************************************/
    if (rc < 0)
    {
       perror("  select() failed");
       break;
    }

    /**********************************************************/
    /* Check to see if the 5 minute time out expired.         */
    /**********************************************************/
    if (rc == 0)
    {
       printf("  select() timed out.  End program.\n");
       break;
    }

    /**********************************************************/
    /* One or more descriptors are readable.  Need to         */
    /* determine which ones they are.                         */
    /**********************************************************/
    desc_ready = rc;
    for (i=0; i <= max_sd  &&  desc_ready > 0; ++i)
    {
       /*******************************************************/
       /* Check to see if this descriptor is ready            */
       /*******************************************************/
       if (FD_ISSET(i, &working_set))
       {
          /****************************************************/
          /* A descriptor was found that was readable - one   */
          /* less has to be looked for.  This is being done   */
          /* so that we can stop looking at the working set   */
          /* once we have found all of the descriptors that   */
          /* were ready.                                      */
          /****************************************************/
          desc_ready -= 1;

          /****************************************************/
          /* Check to see if this is the listening socket     */
          /****************************************************/
          if (i == listen_sd)
          {
             printf("  Listening socket is readable\n");
             /*************************************************/
             /* Accept all incoming connections that are      */
             /* queued up on the listening socket before we   */
             /* loop back and call select again.              */
             /*************************************************/
             do
             {
                /**********************************************/
                /* Accept each incoming connection.  If       */
                /* accept fails with EWOULDBLOCK, then we     */
                /* have accepted all of them.  Any other      */
                /* failure on accept will cause us to end the */
                /* server.                                    */
                /**********************************************/
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

                /**********************************************/
                /* Add the new incoming connection to the     */
                /* master read set                            */
                /**********************************************/
                printf("  New incoming connection - %d\n", new_sd);
                FD_SET(new_sd, &master_set);
                if (new_sd > max_sd)
                   max_sd = new_sd;

                /**********************************************/
                /* Loop back up and accept another incoming   */
                /* connection                                 */
                /**********************************************/
             } while (new_sd != -1);
          }

          /****************************************************/
          /* This is not the listening socket, therefore an   */
          /* existing connection must be readable             */
          /****************************************************/
          else
          {
             printf("  Descriptor %d is readable\n", i);
             /*************************************************/
             /* Receive all incoming data on this socket      */
             /* before we loop back and call select again.    */
             /*************************************************/
            int type = -1;
            rc = recv(i, &type, sizeof(type), 0);
            if (rc < 0)
            {
               if (errno != EWOULDBLOCK)
               {
                  perror("  recv() failed");
                  // TODO: CLOSE CONNECTION
               }
            }

            /**********************************************/
            /* Check to see if the connection has been    */
            /* closed by the client                       */
            /**********************************************/
            if (rc == 0)
            {
               printf("  Connection closed\n");
               // TODO: CLOSE CONNECTION
            }

            /**********************************************/
            /* Data was received                          */
            /**********************************************/
            len = rc;
            printf("  %d bytes received\n", len);

            // Determine message type
            type = ntohl(type);
            switch (type) {
                case REGISTER: {
                    char server_ip[ADDR_SIZE], proc_name[PROC_NAME_SIZE];
                    char const *server_port;
                    int arg_type, port;
                    std::string proc_signature;
                    recv(i, server_ip, sizeof(server_ip), 0);
                    // Receive port as int then convert to string
                    recv(i, &port, sizeof(port), 0);
                    port = ntohl(port);
                    server_port = std::to_string(port).c_str();
                    recv(i, proc_name, sizeof(proc_name), 0);
                    proc_signature = proc_name;
                    do {
                        recv(i, &arg_type, sizeof(arg_type), 0);
                        arg_type = ntohl(arg_type);
                        if (arg_type) {
                            proc_signature += arg_type;
                        }
                    } while (arg_type != 0);
                    register_proc_server(proc_signature, server_ip, server_port);
                    // TODO: DETERMINE WHEN REGISTER_FAILURE
                    int msg;
                    msg = htonl(REGISTER_SUCCESS);
                    send(i, &msg, sizeof(msg), 0);
                    // TODO: DETERMINE WARNING/ERROR CODES FOR BOTH REGISTER_SUC and _FAIL
                    msg = htonl(2);
                    send(i, &msg, sizeof(msg), 0);
                } break;
                case LOC_REQUEST: {
                    char proc_name[PROC_NAME_SIZE];
                    int arg_type;
                    std::string proc_signature;
                    recv(i, proc_name, sizeof(proc_name), 0);
                    proc_signature = proc_name;
                    do {
                        recv(i, &arg_type, sizeof(arg_type), 0);
                        arg_type = ntohl(arg_type);
                        if (arg_type) {
                            proc_signature += arg_type;
                        }
                    } while (arg_type != 0);

                    Server *server = get_proc_server(proc_signature);
                    int msg;
                    if (server) {
                        msg = htonl(LOC_SUCCESS);
                        send(i, &msg, sizeof(msg), 0);
                        char msg_s[ADDR_SIZE];
                        strcpy(msg_s, (server->ip).c_str());
                        send(i, msg_s, sizeof(msg_s), 0);
                        // Send port as int
                        int port = htonl(std::stoi((server->port).c_str()));
                        send(i, (char*)&port, sizeof(port), 0);
                    } else {
                        msg = htonl(LOC_FAILURE);
                        send(i, &msg, sizeof(msg), 0);
                        // TODO: DETERMINE ERROR CODES + DEFINE THEM IN SHARED HEADER
                        msg = htonl(2);
                        send(i, &msg, sizeof(msg), 0);
                    }
                } break;
                case TERMINATE:
                    break;
                default:
                    break;
            }

            // Close socket connection
            // TODO: SHOULD WE ALWAYS CLOSE CONNECTION ON SOCKET AFTER THIS?
            close(i);
            FD_CLR(i, &master_set);
            if (i == max_sd)
            {
               while (FD_ISSET(max_sd, &master_set) == FALSE)
                  max_sd -= 1;
            }
          } /* End of existing connection is readable */
       } /* End of if (FD_ISSET(i, &working_set)) */
    } /* End of loop through selectable descriptors */

  } while (end_server == FALSE);

  /*************************************************************/
  /* Clean up all of the sockets that are open                  */
  /*************************************************************/
  for (i=0; i <= max_sd; ++i)
  {
    if (FD_ISSET(i, &master_set))
       close(i);
  }
}
