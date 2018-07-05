#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define MAX_MSG_SIZE 1024

struct client_connection
{
    int sockfd;
    uint32_t msg_size, recved, sent;
    char msg[MAX_MSG_SIZE+sizeof(uint32_t)];
};

struct context
{
    int epfd, serverfd, load;
};


void read_client (struct context *context, struct client_connection *client);
void write_client (struct context *context, struct client_connection *client);

void accept_client (struct context *context)
{
    int clientfd = accept4 (context->serverfd, NULL, NULL, SOCK_NONBLOCK);

    struct client_connection *connection = malloc (sizeof (struct client_connection));

    connection->sockfd      = clientfd;
    connection->msg_size    = 0;
    connection->recved      = 0;
    connection->sent        = 0;

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = connection;
    epoll_ctl (context->epfd, EPOLL_CTL_ADD, clientfd, &event);
}

void destroy_client (struct client_connection *client)
{
    /*
     *  EPoll events are deleted automatically when the fd is closed.
     */
    close (client->sockfd);
    free (client);
}

void do_work (struct context *context, struct client_connection *client)
{
    int x = 0;
    for (int i = 0; true; i++)
    {
        x += i * i;

        if (x > context->load)
        {
            break ;
        }
    }

    // use that value, dont optimize out
    client->msg[5] = x;

    write_client(context, client);
}

void write_client (struct context *context, struct client_connection *client)
{
    while (true)
    {
        ssize_t ret = write (client->sockfd, client->msg + client->sent, client->msg_size + 4 - client->sent);

        if (ret < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break ;
            }
            else
            {
                fprintf(stderr, "Write failed: %s (%d)\n", strerror(errno), errno);
                destroy_client(client);
                return ;
            }
        }
        else
        {
            client->sent += ret;

            if (client->sent = client->msg_size + 4)
            {
                // done sending
                client->recved = 0;
                read_client (context, client);
                return ;
            }
        }
    }

    struct epoll_event event;
    event.events = EPOLLOUT | EPOLLONESHOT;
    event.data.ptr = client;
    epoll_ctl(context->epfd, EPOLL_CTL_MOD, client->sockfd, &event);

}

void read_client (struct context *context, struct client_connection *client)
{
    while (true)
    {
        ssize_t ret = read (client->sockfd, client->msg + client->recved, MAX_MSG_SIZE - client->recved);

        if (ret < 0)
        {

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break ;
            }
            else
            {
                fprintf(stderr, "Read failed: %s (%d)\n", strerror(errno), errno);
                destroy_client(client);
                return ;
            }
        }
        else if (ret == 0)
        {
            destroy_client(client);
            return ;
        }
        else
        {
            client->recved += ret;

            if (client->recved > 4)
            {
                memcpy (&client->msg_size, client->msg, 4);

                if (client->recved = client->msg_size + 4)
                {
                    // send it back
                    client->sent = 0;
                    do_work (context, client);
                    return ;
                }
            }
        }
    }

    // update epoll
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLONESHOT;
    event.data.ptr = client;
    epoll_ctl(context->epfd, EPOLL_CTL_MOD, client->sockfd, &event);
}

void handle_client (struct context *context, struct client_connection *client, struct epoll_event *event)
{

    if (event->events & (EPOLLHUP | EPOLLERR))
    {
        destroy_client (client);
    }
    else if (event->events & EPOLLIN)
    {
        // read the socket
        read_client (context, client);
    }
    else if (event->events & EPOLLOUT)
    {
        write_client (context, client);
    }
    else
    {
        // WTF?
        destroy_client(client);
    }
}

void *thread_routine (void *user)
{
    // copy context, epfd and serverfd wont change anymore
    struct context context = *(struct context*) user;

    struct epoll_event events[2], *event;

    while (true)
    {
        int num_events = epoll_wait(context.epfd, events, sizeof(events)/sizeof(struct epoll_event), -1);

        if (num_events < 0)
        {
            break ;
        }

        for (int i = 0; i < num_events; i++)
        {
            event = &events[i];

            if (event->data.ptr == NULL)
            {
                // this is the server socket
                accept_client (&context);
            }
            else if (event->data.u64 == (uint64_t) -1)
            {
                // stop event triggered
                return NULL;
            }
            else
            {
                // this is a client socket
                struct client_connection *client = (struct client_connection*) event->data.ptr;

                handle_client(&context, client, event);
            }
        }
    }

    puts ("Thread stopped.");
}

int stop_event_fd;

void signal_handler (int signal)
{
    uint64_t signaled = 1;
    while (true)
    {
        int ret = write(stop_event_fd, &signaled, sizeof(uint64_t));

        if (ret < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue ;
            }

            fprintf(stderr, "Error writing event fd: %s (%d)\n", strerror(errno), errno);
        }

        break;
    }

    puts("Signaled stop.");
}

void print_help ()
{
    puts ("Options (Defaults)");
    puts ("-p Port (4568)");
    puts ("-n Number of threads (8)");
    puts ("-l Load (25000)");
}

int main (int argc, char *argv[])
{

    in_port_t port = 4568;
    int num_threads = 8, opt, load = 25000;

    while ((opt = getopt (argc, argv, "l:p:n:h")) != -1)
    {
        //printf ("opt = %c (%x), arg = %s\n", opt, opt, optarg);
        switch (opt)
        {
            case 'p':
                port = atoi (optarg);
                break ;

            case 'n':
                num_threads = atoi(optarg);
                break ;

            case 'l':
                load = atoi(optarg);
                break ;

            case 'h':
                print_help();
                return EXIT_SUCCESS;
            case '?':
            default:
                fprintf (stderr, "Bad parameters.\n");
                print_help();
                return EXIT_FAILURE;
        }
    }

    if (port == 0)
    {
        fprintf (stderr, "Invalid port.\n");
        return EXIT_FAILURE;
    }

    if (num_threads == 0)
    {
        fprintf(stderr, "Invalid number of threads.\n");
        return EXIT_FAILURE;
    }

    /*
     *  Open a socket, bind and listen. Then create multiple threads that all
     *  wait on the same epoll. Use EPOLLONESHOT to receive events.
     *
     */

    int sockfd = socket (AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1)
    {
        fprintf (stderr, "Failed to create socket: %s (%d)\n", strerror (errno), errno);
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr_in;
    addr_in.sin_family      = AF_INET;
    addr_in.sin_port        = htons (port);
    addr_in.sin_addr.s_addr = INADDR_ANY;

    if (bind (sockfd, (struct sockaddr *) &addr_in, sizeof (addr_in)) != 0)
    {
        fprintf (stderr, "Failed to bind socket: %s (%d)\n", strerror (errno), errno);
        return EXIT_FAILURE;
    }

    if (listen (sockfd, SOMAXCONN) != 0)
    {
        fprintf (stderr, "Failed to listen on socket: %s (%d)\n", strerror (errno), errno);
        return EXIT_FAILURE;
    }


    /*
     *  Install signal handler.
     */
    signal (SIGINT, &signal_handler);

    /*
     *  Create the epoll instance
     */
    int epollfd = epoll_create(1);

    /*
     *  Create stop event and add it to epoll.
     */
    stop_event_fd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event eevent;
    eevent.events   = EPOLLIN;
    eevent.data.u64 = (uint64_t) -1;
    epoll_ctl (epollfd, EPOLL_CTL_ADD, stop_event_fd, &eevent);

    /*
     *  Add the server socket to the epoll instance.
     */
    eevent.events   = EPOLLIN;  // on client only
    eevent.data.ptr = NULL;
    epoll_ctl (epollfd, EPOLL_CTL_ADD, sockfd, &eevent);

    struct context context;
    context.serverfd    = sockfd;
    context.epfd        = epollfd;
    context.load        = load;


    /* Initialize and set thread detached attribute */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);



    /*
     *  Create threads.
     */

    pthread_t threads[num_threads];

    printf ("Starting %d threads.\n", num_threads);

    for (int i = 0; i < num_threads; i++)
    {
        pthread_create (&threads[i], &attr, thread_routine, &context);
    }
    pthread_attr_destroy(&attr);


    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    puts ("Exit main thread");

    return EXIT_SUCCESS;
}
