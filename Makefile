librpc.a: librpc.o
	ar -rcs librpc.a librpc.o
	make clean

librpc.o: librpc.cpp rpc.h
	g++ -c librpc.cpp

binder: binder.o
	g++ -o binder binder.o

binder.o: binder.cpp
	g++ -c binder.cpp

clean:
	rm -f *.o


