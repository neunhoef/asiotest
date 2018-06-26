all: asio_server asio_client

asio_server: asio_server.cpp asio_client_server.h
	g++ -Wall -O3 -g -o asio_server asio_server.cpp -std=c++11 -lpthread -I asio/asio/include

asio_client: asio_client.cpp asio_client_server.h
	g++ -Wall -O3 -g -o asio_client asio_client.cpp -std=c++11 -lpthread -I asio/asio/include

getasio:
	git clone https://github.com/chriskohlhoff/asio

clean:
	rm -rf asio_client asio_server
