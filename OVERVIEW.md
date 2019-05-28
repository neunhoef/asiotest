calibrate.cpp:
  calibrate delay number

---------------------------------------------------------------

server.cpp:
  variable number of IO threads, each one io_context
  variable number of worker threads, all in one io_context
  no other scheduler
  variable length client requests, server simply echos
  io threads execute directly

Example:

  ./server 8000 1 1 2869081
  # for 1ms work and 1 io thread and 1 worker (delay from calibrate)
  ./client ./client localhost 8000 1000 1 20
  # for 1000 requests, one thread and 20 bytes payload

---------------------------------------------------------------

server2.cpp:
  variable number of IO threads, each one io_context
  variable number of worker threads in thread farm
  variable length client requests, 
  worker farm with deque for queue

Example:

  ./server2 8000 1 1 2869081
  # for 1ms work and 1 io thread and 1 worker (delay from calibrate)
  ./client2 localhost 8000 10000 1 20
  # for 1ms work and 1 thread and 20 bytes payload

---------------------------------------------------------------

server3.cpp:

  Richard worker farm, variable number of io threads, variable number of workers
  deque as work queue
  futex based spin_lock by Manuel for locking of deque
  
Example:

  ./server3 8000 1 1 2869081
  # for 1ms work and 1 io thread and 1 worker (delay from calibrate)
  ./client2 localhost 8000 10000 1 20
  # for 1ms work and 1 thread and 20 bytes payload

---------------------------------------------------------------

server_generic.cpp:

  Generic worker farm, multiple implementations:
    1 - Richard with locks
    2 - Futex (Manuel)
    3 - Richard lock free
    4 - std lockfree
    5 - adv lockfree
  
Example:

  ./server-generic 8000 1 1 2869081 3
  # for 1ms work and 1 io thread and 1 worker (delay from calibrate), method 3
  ./client2 localhost 8000 10000 1 20
  # for 1ms work and 1 thread and 20 bytes payload

server_generic2.cpp is slower but adds a method

---------------------------------------------------------------

server_final.cpp:

  this adds starting and stopping threads
  it offers ssl and two different worker farms

Example:

  TOBEDONE, needs client4 and this does
  does not seem to work with client2


