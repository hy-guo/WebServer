#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include "thread_pool.h"
#include "http_conn.h"

#define MAX_FD 65534           // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 60000 // 监听的最大的事件数量

int main(int argc, char *argv[])
{
    int port=80, num_threads = NUM_THREADS;

    if (argc > 1)
        port = atoi(argv[1]);
    printf("Use Port %d\n", port);
    
    if (argc > 2)
        num_threads = atoi(argv[2]);

    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    int listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listen_fd >= 0);

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) // 端口复用
        exit(-1);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0)
        exit(-1);
    if (listen(listen_fd, 5) != 0)
        exit(-1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epoll_fd = epoll_create(1);

    epoll_event listen_event;
    listen_event.data.fd = listen_fd;
    listen_event.events = EPOLLIN;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listen_event);

    http_conn::m_user_count = 0;
    http_conn::m_epoll_fd = epoll_fd;
    http_conn *users = new http_conn[MAX_FD];
    thread_pool<http_conn> *pool = new thread_pool<http_conn>(num_threads);

    while (true)
    {
        int events_num = epoll_wait(epoll_fd, events, MAX_EVENT_NUMBER, -1);

        if (events_num < 0 && errno != EINTR)
        {
            printf("Epoll Error!\n");
            break;
        }
        for (int i = 0; i < events_num; i++)
        {

            int sock_fd = events[i].data.fd;

            if (sock_fd == listen_fd)
            {

                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int conn_fd = accept(listen_fd, (struct sockaddr *)&client_address, &client_addrlength);

                if (conn_fd < 0)
                {
                    printf("Accept Error! Errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    close(conn_fd);
                    continue;
                }
                users[conn_fd].init(conn_fd, client_address);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sock_fd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                if (users[sock_fd].read())
                {
                    pool->append(users + sock_fd);
                }
                else
                {
                    users[sock_fd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                if (!users[sock_fd].write())
                {
                    users[sock_fd].close_conn();
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
    delete[] users;
    delete pool;
    return 0;
}
