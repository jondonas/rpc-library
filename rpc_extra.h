#include <vector>
#include<tuple>

// Type values for messages
#define REGISTER            1
#define LOC_REQUEST         2
#define LOC_SUCCESS         3
#define LOC_FAILURE         4
#define EXECUTE             5
#define EXECUTE_SUCCESS     6
#define EXECUTE_FAILURE     7
#define TERMINATE           8

int sock_client, sock_binder, client_port;
char server_ip[65];

std::vector <std::tuple<char *, int *, skeleton>> database;