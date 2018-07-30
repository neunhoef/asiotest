all: asio_server asio_client asio_server_varlen asio_client_varlen client server client2 server2 \
	server3 server4 calibrate worker_test server3-lf eponeshots server-generic client4  \
	client4-ssl server-final
#server-generic2-ssl server-generic2

CPPFLAGS = -Wall -O3 -g -march=native -DASIO_DISABLE_NOEXCEPT
#CPPFLAGS = -Wall -O0 -g

asio_server: asio_server.cpp asio_client_server.h Makefile
	g++ ${CPPFLAGS} -o asio_server asio_server.cpp -std=c++11 -lpthread -I asio/asio/include

asio_client: asio_client.cpp asio_client_server.h Makefile
	g++ ${CPPFLAGS} -o asio_client asio_client.cpp -std=c++11 -lpthread -I asio/asio/include

server: server.cpp Makefile
	g++ ${CPPFLAGS} -o server server.cpp -std=c++11 -lpthread -I asio/asio/include

client: client.cpp Makefile
	g++ ${CPPFLAGS} -o client client.cpp -std=c++11 -lpthread -I asio/asio/include

server2: server2.cpp Makefile
	g++ ${CPPFLAGS} -o server2 server2.cpp -std=c++11 -lpthread -I asio/asio/include

server3: server3.cpp Makefile worker_farm.h richard_worker_farm.h
	g++ ${CPPFLAGS} -o server3 server3.cpp -std=c++11 -lpthread -I asio/asio/include

server4: server4.cpp Makefile worker_farm.h futex_worker_farm.h
	g++ ${CPPFLAGS} -o server4 server4.cpp -std=c++11 -lpthread -I asio/asio/include

server3-lf: server3-lf.cpp Makefile worker_farm.h lockfree_richard_worker_farm.h
	g++ ${CPPFLAGS} -o server3-lf server3-lf.cpp -std=c++11 -lpthread -I asio/asio/include

worker_test: worker_test.cpp Makefile worker_farm.h lockfree_richard_worker_farm.h richard_worker_farm.h futex_worker_farm.h \
	std_worker_farm.hpp manuel-worker-farm.hpp
	g++ ${CPPFLAGS} -o worker_test worker_test.cpp -std=c++11 -lpthread

client2: client2.cpp Makefile
	g++ ${CPPFLAGS} -o client2 client2.cpp -std=c++11 -lpthread -I asio/asio/include

client3: client3.cpp Makefile
	g++ ${CPPFLAGS} -o client3 client3.cpp -std=c++11 -lpthread -I asio/asio/include

client4: client4.cpp Makefile
	g++ ${CPPFLAGS} -o client4 client4.cpp -std=c++11 -lpthread -I asio/asio/include

client4-ssl: client4-ssl.cpp Makefile
	g++ ${CPPFLAGS} -o client4-ssl client4-ssl.cpp -std=c++11 -lpthread -I asio/asio/include -lssl -lcrypto


server-generic: server-generic.cpp Makefile worker_farm.h lockfree_richard_worker_farm.h richard_worker_farm.h futex_worker_farm.h adv-worker-farm.hpp manuel-worker-farm.hpp
	g++ ${CPPFLAGS} -o server-generic server-generic.cpp -std=c++11 -lpthread -I asio/asio/include

#server-generic2: server-generic2.cpp Makefile worker_farm.h lockfree_richard_worker_farm.h richard_worker_farm.h futex_worker_farm.h adv-worker-farm.hpp
#	g++ ${CPPFLAGS} -o server-generic2 server-generic2.cpp -std=c++11 -lpthread -I asio/asio/include

server-final: server-final.cpp Makefile worker_farm.h adv-worker-farm.hpp best-worker-farm.hpp
	g++ ${CPPFLAGS} -o server-final server-final.cpp -std=c++11 -lpthread -I asio/asio/include -lssl -lcrypto

#server-generic2-ssl: server-generic2-ssl.cpp Makefile worker_farm.h lockfree_richard_worker_farm.h richard_worker_farm.h futex_worker_farm.h
#	g++ ${CPPFLAGS} -o server-generic2-ssl server-generic2-ssl.cpp -std=c++11 -lpthread -I asio/asio/include -lssl -lcrypto

eponeshots: epoll_oneshot_server.c Makefile
	gcc -O3 -march=native -o eponeshots epoll_oneshot_server.c -lpthread

asio_server_varlen: asio_server_varlen.cpp Makefile
	g++ ${CPPFLAGS} -o asio_server_varlen asio_server_varlen.cpp -std=c++11 -lpthread -I asio/asio/include

asio_client_varlen: asio_client_varlen.cpp Makefile
	g++ ${CPPFLAGS} -o asio_client_varlen asio_client_varlen.cpp -std=c++11 -lpthread -I asio/asio/include

calibrate: calibrate.cpp Makefile
	g++ ${CPPFLAGS} -o calibrate calibrate.cpp -std=c++11 -lpthread

getasio:
	git clone https://github.com/chriskohlhoff/asio

clean:
	rm -rf asio_server asio_client asio_server_varlen asio_client_varlen client server client2 server2 \
	server3 server4 calibrate worker_test server3-lf eponeshots server-generic client4 \
	client4-ssl server-generic2-ssl server-final
