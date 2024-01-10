#include "include/threadpool.h"
#include "./timer/srp_timer.h"
#include <cassert>

#define MAX_FD 65535           // 最大文件描述符数
#define MAX_EVENT_NUMBER 10000 // 监听的最大事件数量
#define TIMESLOT 5             // 单位时间

static int pipefd[2];            // noactive的管道
static sort_timer_srp timer_srp; // noactive的容器

// 定义在httpConn.cpp中
// 添加监听的文件描述符相关的检测信息到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);
// 设置fd非阻塞
extern int setnonblocking(int fd);

// 添加信号捕捉
// handler：回调函数
void addSig(int sig, void(handler)(int))
{
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask); // 清空临时阻塞信号集
    sa.sa_handler = handler;

    //????
    // sigfillset(&sa.sa_mask);

    // 注册新的信号捕捉
    // 捕捉到该新信号就调用回调函数handler
    sigaction(sig, &sa, NULL);
}

// noactive向管道发送信号
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;

    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 当时间到时，处理非活跃用户
void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_srp.tick();

    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func(HttpConn *user_data)
{
    user_data->close_conn();
}

int main(int argc, char *argv[])
{

    if (argc <= 1)
    { // 运行时加上端口号
        // basename：用于去除路径和文件后缀部分的文件名，只获取执行程序名称
        // eg：./server 8080
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    // atoi:字符串转换成整型数
    int port = atoi(argv[1]);

    /* 对SIGPIPE信号进行处理
       当 client 连接到 server 之后,
       这时候 server 准备向 client 发送多条消息
       但在发送消息之前，client 进程意外崩溃了
       那么接下来 server 在发送多条消息的过程中，就会出现SIGPIPE信号
       为了主线程不被这个信号错误杀死，需要设置信号捕捉忽略该信号
    */
    // SIGPIPE的回调函数：SIG_IGN，ignore忽略
    addSig(SIGPIPE, SIG_IGN);

    // 程序运行就创建线程池并初始化
    // 任务类：HttpConn连接类
    ThreadPool<HttpConn> *pool = NULL;
    // 异常捕捉
    try
    {
        pool = new ThreadPool<HttpConn>;
    }
    catch (...)
    {
        return 1;
    }

    // 创建一个数组用于保存所有连接过来的客户端信息
    HttpConn *users = new HttpConn[MAX_FD];

    /*
     * 网络模块
     */

    // 创建用于监听的socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); // IPv4;流式

    if (listenfd == -1)
    {
        perror("socket");
        exit(-1);
    }

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    // TCP/IP 协议族有 sockaddr_in 和 sockaddr_in6 两个专用的 socket 地址结构体，它们分别用于 IPv4 和 IPv6
    struct sockaddr_in address; // 存储服务器定义的ip + port信息

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 0.0.0.0 表示任意地址
    // inet_pton(AF_INET, "192.168.56.101", &address.sin_addr.s_addr); //本机的IP地址，实际开发时就是域名中的ip地址
    address.sin_port = htons(port); // 主机转网络字节序

    // 和本机的ip+port绑定
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));

    if (ret == -1)
    {
        perror("bind");
        exit(-1);
    }

    // 监听
    ret = listen(listenfd, 5);
    if (ret == -1)
    {
        perror("listen");
        exit(-1);
    }

    // 调用epoll_create()创建一个epoll实例
    int epollfd = epoll_create(100);

    // events[]:传出数组，保存发生了监听事件的数组，用于用户态操作
    struct epoll_event events[MAX_EVENT_NUMBER];

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);

    // 所有的socket上的事件都被注册到同一个epoll对象中
    HttpConn::m_epollfd = epollfd;

    // 创建管道 noactive-1 创建一个两端通信的管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    // 设置信号处理函数 noactive-2 SIGALRM定时器信号，SIGTERM进程终止信号
    addSig(SIGALRM, sig_handler);
    addSig(SIGTERM, sig_handler);
    bool stop_server = false;
    bool timeout = false;

    alarm(TIMESLOT); // 定时,5秒后产生SIGALARM信号

    // 循环检测事件发生
    while (true)
    {
        printf("timer_srp.size = %d,ref_ size = %d\n", timer_srp.getsize_(), timer_srp.getrefsize());
        printf("当前用户数:%d\n", HttpConn::m_user_count);

        // num：epoll监听到发生了事件的个数
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((num < 0) && (errno != EINTR))
        {
            perror("epoll_wait");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++)
        {

            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 监听的文件描述符有数据达到，有客户端连接
                // 将客户端fd添加到epev
                // client_address传出参数，保存着客户端的信息(ip + port)
                // connfd:用于与该客户端通信的文件描述符，accept返回值
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);

                if (HttpConn::m_user_count >= MAX_FD)
                {
                    // 目前连接满
                    // todo：给客户端写信息，说服务器繁忙，响应报文
                    close(connfd);
                    continue;
                }

                // 将新的客户数据初始化，放到数组中，将connfd添加到epoll对象中
                users[connfd].init(connfd, client_address);

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中

                printf("新用户connfd = %d\n", connfd);

                TimerNode *timer = new TimerNode;
                timer->user_data = &users[connfd];  // 用户信息
                timer->cb_func = cb_func;           // 回调函数
                time_t cur = time(NULL);            // 当前时间
                timer->expire = cur + 3 * TIMESLOT; // 设置失效时间
                users[connfd].timer = timer;        // 设置定时器
                timer_srp.add_timer(timer);
                printf("向timer中添加fd = %d\n", connfd);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);

                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                TimerNode *timer = users[sockfd].timer;
                cb_func(&users[sockfd]);
                if (timer)
                {
                    timer_srp.del_timer(timer);
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                TimerNode *timer = users[sockfd].timer;
                // 如果是读事件
                if (users[sockfd].read())
                {
                    pool->append(users + sockfd);
                    // 延迟该连接被关闭的时间
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 2 * TIMESLOT;
                        printf("adjust timer once\n");
                        timer_srp.adjust_timer(timer); // 调整失效时间
                    }
                }
                else
                {
                    cb_func(&users[sockfd]);
                    if (timer)
                    {
                        timer_srp.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                // 如果是写事件
                if (!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }

    close(pipefd[1]);
    close(pipefd[0]);
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
}