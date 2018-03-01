#include <string>
#include <map>
#include <vector>


/***
 * CLASSES/STRUCTS
 ***/

struct Server
{
  std::string ip;
  std::string port;
};

class Proc
{
  int current_server;
  public:
    std::vector<Server> servers;
    Server *get_next_available_server();
};

Server *Proc::get_next_available_server()
{
  if (servers.size() == 0) return NULL;
  if (current_server >= servers.size()) current_server = 0;
  Server *server = &(servers[current_server]);
  if (last_used_server != NULL && server->ip == last_used_server->ip) {
    current_server = (current_server + 1) % servers.size();
    server = &(servers[current_server]);
  }
  current_server = (current_server + 1) % servers.size();
  last_used_server = server;
  return server;
}

/***
 * CONSTANTS
 ***/

std::map<string, std::vector<Proc>> PROCS;
Server *last_used_server = NULL;

/***
 * BINDER
 ***/
  
// Maps a procedure name with a server
void register_proc_server(string proc_name, string server_ip, string server_port)
{
  Server server = Server(server_ip, server_port);
  Proc proc = PROCS[proc_name];
  proc.servers.push_back(server);
}

// Removes a mapping between a procedure name and a server
void remove_proc_server(string server_ip)
{
  for (std::map<string, Proc>::iterator it = PROCS.begin(); it != PROCS.end(); ++it)
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
Server *get_proc_server(string proc_name)
{
  return PROCS[proc_name].get_next_available_server();
}

int main()
{
  // TODO: Write socket code to allow clients/servers to communicate with binder.

  // TODO: Use lock to prevent changes to PROCS from multiple threads at once.
}
