all: binder librpc.a

librpc.a: librpc.o
	ar -rcs librpc.a librpc.o
	make clean

librpc.o: librpc.cpp rpc.h rpc_extra.h
	g++ -c librpc.cpp -std=c++11

binder: binder.o
	g++ -o binder binder.o

binder.o: binder.cpp
	g++ -c binder.cpp -std=c++11

clean:
	rm -f *.o


