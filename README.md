# asio library test for a TCP server

This is a small test program to see what the principle limits are
when writing a TCP server with the asio library.

The server is a simple echo server with a constant message size.
It can use a configurable number of threads. The client is a single
process which can also use multiple threads, each using one connection
to the server. The client uses blocking I/O operations and no I/O
multiplexing.
