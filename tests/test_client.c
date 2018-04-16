#include "rpc.h"
#include <stdio.h>

int main() {

  char a = 'a';
  short b = 1;
  int c = 2;
  long d = 3;
  double e = 4;
  float f = 5.0;
  int return0 = -1;
  char *msg = "testme";
  int argTypes[8];
  void **args;

  argTypes[0] = (1 << ARG_OUTPUT) | (ARG_INT << 16);
  argTypes[1] = (1 << ARG_INPUT) | (ARG_CHAR << 16);
  argTypes[2] = (1 << ARG_INPUT) | (ARG_SHORT << 16);
  argTypes[3] = (1 << ARG_INPUT) | (ARG_INT << 16);
  argTypes[4] = (1 << ARG_INPUT) | (ARG_LONG << 16);
  argTypes[5] = (1 << ARG_INPUT) | (ARG_DOUBLE << 16);
  argTypes[6] = (1 << ARG_INPUT) | (ARG_FLOAT << 16);
  argTypes[7] = (1 << ARG_INPUT) | (ARG_CHAR << 16) | 7;
  argTypes[8] = 0;
    
  args = (void **)malloc(8 * sizeof(void *));
  args[0] = (void *)&return0;
  args[1] = (void *)&a;
  args[2] = (void *)&b;
  args[3] = (void *)&c;
  args[4] = (void *)&d;
  args[5] = (void *)&e;
  args[6] = (void *)&f;
  args[6] = (void *)msg;

  int s = rpcCall("f0", argTypes, args);
  if (s >= 0) { 
    printf("Test returned: %ld\n", *((int *)(args[0])));
  }
  else {
    printf("Error: %d\n", s);
  }
}
