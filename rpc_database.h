#include <vector>
#include <tuple>
#include <string>

int sock_client, sock_binder, client_port;
bool running = true;
char server_ip[65];

std::vector <std::tuple<char *, int *, skeleton>> database;
std::vector <std::tuple<char *, int *, std::vector<std::tuple<std::string, int>>>> cached_procs;