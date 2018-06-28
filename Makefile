all: asio_server asio_client asio_server_varlen asio_client_varlen client server

CPPFLAGS = -Wall -O3 -g
#CPPFLAGS = -Wall -O0 -g

asio_server: asio_server.cpp asio_client_server.h Makefile
	g++ ${CPPFLAGS} -o asio_server asio_server.cpp -std=c++11 -lpthread -I asio/asio/include

asio_client: asio_client.cpp asio_client_server.h Makefile
	g++ ${CPPFLAGS} -o asio_client asio_client.cpp -std=c++11 -lpthread -I asio/asio/include

server: server.cpp Makefile
	g++ ${CPPFLAGS} -o server server.cpp -std=c++11 -lpthread -I asio/asio/include

client: client.cpp Makefile
	g++ ${CPPFLAGS} -o client client.cpp -std=c++11 -lpthread -I asio/asio/include

asio_server_varlen: asio_server_varlen.cpp Makefile
	g++ ${CPPFLAGS} -o asio_server_varlen asio_server_varlen.cpp -std=c++11 -lpthread -I asio/asio/include

asio_client_varlen: asio_client_varlen.cpp Makefile
	g++ ${CPPFLAGS} -o asio_client_varlen asio_client_varlen.cpp -std=c++11 -lpthread -I asio/asio/include

getasio:
	git clone https://github.com/chriskohlhoff/asio

clean:
	rm -rf asio_client asio_server asio_server_varlen asio_client_varlen client server
