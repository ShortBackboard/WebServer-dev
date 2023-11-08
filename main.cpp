/*
 *
    流程：
        1.主线程监听到有新的客户端连接，将accept()的返回的客户端信息添加到HttpConn类型的users[MAX_FD]数组中
        HttpConn *users = new HttpConn[MAX_FD];
        users[connfd].init(connfd, client_address);

        2.将用于与该客户端通信的fd添加到epoll对象中并设置要监听的事件

        3.主线程循环while运行等待epoll监听事件

        4.如果是EPOLLIN事件，说明已经连接的某个客服端发送了请求报文，将请求报文全部一次性read()完毕
        并调用线程池的append()函数push_back()到请求队列，
        此时线程池中的某个线程取得请求队列的数据，进行run()函数内的process()函数对请求报文进行解析

        process()函数对请求报文解析完毕后，会生成响应，并将该通信的socketfd事件改为EPOLLOUT

        5.epoll监听到EPOLLOUT事件后，就调用write()将生成的响应报文发送给客户端
         浏览器再自动解析响应报文字段里面的.html文件等并显示


    注意：
        1.epoll只负责监听读写事件，解析和生成报文由用户态实现
*/


#include "include/threadpool.h"

#define MAX_FD 65535 //最大文件描述符数
#define MAX_EVENT_NUMBER 10000 //监听的最大事件数量



// 添加信号捕捉
// handler：回调函数
void addSig(int sig, void(handler)(int))
{
    struct sigaction sa;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask); // 清空临时阻塞信号集
    sa.sa_handler = handler;

    //????
    //sigfillset(&sa.sa_mask);

    // 注册新的信号捕捉
    // 捕捉到该新信号就调用回调函数handler
    sigaction(sig, &sa, NULL);
}

// 定义在httpConn.cpp中
// 添加监听的文件描述符相关的检测信息到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);


int main(int argc, char *argv[])
{

    if(argc <= 1) {// 运行时加上端口号
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
    try {
        pool = new ThreadPool<HttpConn>;
    } catch(...) {
        return 1;
    }

    // 创建一个数组用于保存所有连接过来的客户端信息
    HttpConn *users = new HttpConn[MAX_FD];


    /*
     * 网络模块
     */

    // 创建用于监听的socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);//IPv4;流式

    if(listenfd == -1) {
        perror("socket");
        exit(-1);
    }

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    // TCP/IP 协议族有 sockaddr_in 和 sockaddr_in6 两个专用的 socket 地址结构体，它们分别用于 IPv4 和 IPv6
    struct sockaddr_in address; //存储服务器定义的ip + port信息

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;  // 0.0.0.0 表示任意地址
    // inet_pton(AF_INET, "192.168.56.101", &address.sin_addr.s_addr); //本机的IP地址，实际开发时就是域名中的ip地址
    address.sin_port = htons(port);// 主机转网络字节序

    // 和本机的ip+port绑定
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));

    if(ret == -1) {
        perror("bind");
        exit(-1);
    }

    // 监听
    ret = listen(listenfd, 5);
    if(ret == -1) {
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

    // 循环检测事件发生
    while (true)
    {
        // num：epoll监听到发生了事件的个数
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num < 0) && (errno != EINTR)) {
            perror("epoll_wait");
            break;
        }

        // 循环遍历事件数组
        for(int i = 0; i < num; i++) {

            int sockfd = events[i].data.fd;

            if(sockfd == listenfd) {
                // 监听的文件描述符有数据达到，有客户端连接
                // 将客户端fd添加到epev
                // client_address传出参数，保存着客户端的信息(ip + port)
                // connfd:用于与该客户端通信的文件描述符，accept返回值
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);

                if(HttpConn::m_user_count >= MAX_FD) {
                    // 目前连接满
                    // todo：给客户端写信息，说服务器繁忙，响应报文
                    close(connfd);
                    continue;
                }

                // 将新的客户数据初始化，放到数组中，将connfd添加到epoll对象中
                users[connfd].init(connfd, client_address);

            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 客户端异常断开或错误事件
                users[sockfd].close_conn();

            }else if(events[i].events & EPOLLIN) {
                // 读事件，客户端有请求报文发送
                if(users[sockfd].read()) {
                    // 一次性把数据都读出来
                    // users数组首地址加sockfd偏移量
                    pool->append(users + sockfd);
                }else {
                    users[sockfd].close_conn();
                }

            }else if(events[i].events & EPOLLOUT) {
                //写事件，给客户端生成响应报文
               if(!users[sockfd].write()) {//一次性写完
                    users[sockfd].close_conn();
               }
            }
        }
    }

    close(epollfd);
    close(listenfd);

    delete []users;
    delete pool;

    return 0;
}


