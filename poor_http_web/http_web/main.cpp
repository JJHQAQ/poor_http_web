//主函数
//使用线程池实现的web服务器
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>

#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction ac;
    memset(&ac, '\0', sizeof(ac));
    ac.sa_handler = handler;
    if(restart)
    {
        ac.sa_flags |= SA_RESTART;//中断系统调用时，返回的时候重新执行该系统调用
    }
    sigfillset(&ac.sa_mask);//sa_mask中的信号只有在该ac指向的信号处理函数执行的时候才会屏蔽的信号集合!
    assert(sigaction(sig, &ac, NULL) != -1);
}

//显示错误，关闭该链接的文件描述符
void show_error(int connfd, const char * info)
{
    printf("%s",info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char* argv[])
{
    if(argc <= 1)
    {
        printf("usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }
    //const char* ip = argv[1];
    int port = atoi(argv[1]);
    
    //忽略SIGPIPE信号,SIG_IGN表示忽略信号的宏
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池
    threadpool<http_conn> * pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(10,20);
    }
    catch(...)
    {
        return 1;
    }

    //预先为每个可能的客户链接分配一个http_conn对象
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int users_conut = 0;

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);


    struct linger tmp = {1, 0};
    /*设置优雅断开
    #include <arpa/inet.h>
    struct linger {
　　    int l_onoff;
　　    int l_linger;
    };
    三种断开方式：

    1. l_onoff = 0; l_linger忽略
    close()立刻返回，底层会将未发送完的数据发送完成后再释放资源，即优雅退出。

    2. l_onoff != 0; l_linger = 0;
    close()立刻返回，但不会发送未发送完成的数据，而是通过一个REST包强制的关闭socket描述符，即强制退出。

    3. l_onoff != 0; l_linger > 0;
    close()不会立刻返回，内核会延迟一段时间，这个时间就由l_linger的值来决定。如果超时时间到达之前，发送
    完未发送的数据(包括FIN包)并得到另一端的确认，close()会返回正确，socket描述符优雅性退出。否则，close()
    会直接返回错误值，未发送数据丢失，socket描述符被强制性退出。需要注意的时，如果socket描述符被设置为非堵
    塞型，则close()会直接返回值。
    */
 
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));//设置端口复用

    int ret = 0;
    struct sockaddr_in  address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= -1);

    ret = listen(listenfd, 5);
    assert(ret >= -1);

    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;


    printf("while！\n");
    while(true)
    {
        printf("epoll wait!\n");
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0) && (errno != EINTR))
        {
            printf("epoll failure!\n");
            break;
        }

        for(int i = 0; i < number; ++i)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t clinet_addrlength = sizeof(client_address);
                printf("wait listen accpt!\n");
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, & clinet_addrlength);
                printf("get listen accpt!\n");
            
                if(connfd < 0)
                {
                    printf("error is %d\n",errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd,"Internet server busy");
                    continue;
                }
                //初始化客户端链接
                users[connfd].init(connfd, client_address);

            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                printf("accpt error!\n");
                //如果有异常直接关闭链接
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN)
            {
                printf("wait epollin accpt!\n");
                if(users[sockfd].read())
                {
                    pool->append(users + sockfd);//直接吧整个http_conn都传进去，不过传的都是地址哈，+sockfd就是传当前sock所在的对象呀
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                printf("wait epollout accpt!\n");
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
            else{}
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;


}