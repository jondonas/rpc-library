librpc.a: librpc.o
	ar -rcs librpc.a librpc.o
	make clean

librpc.o: librpc.cpp rpc.h
	g++ -c librpc.cpp

clean:
	rm -f *.o


