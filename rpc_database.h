#include <vector>
#include <tuple>

int sock_client, sock_binder, client_port;
bool running = true;
char server_ip[65];

std::vector <std::tuple<char *, int *, skeleton>> database;
