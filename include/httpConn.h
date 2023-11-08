//  http连接类，保存一个连接客户端的信息

#ifndef HTTPCONN_H
#define HTTPCONN_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>

#define READ_BUFFER_SIZE 2048  //读缓冲区的大小
#define WRITE_BUFFER_SIZE 1024 //写缓冲区的大小
#define FILENAME_LEN 200       // 文件名的最大长度

// 有限状态机的枚举状态:
// HTTP请求方法，本项目只支持GET，后面自己实现POST
enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};


/*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };

// 从状态机状态：在解析每一行时的状态
// 从状态机的三种可能状态，即行的读取状态，分别表示
// 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整，还没有检测完
enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };


/*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST,
                 FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };


class HttpConn {
public:
    HttpConn(){}

    ~HttpConn(){}

    // 处理客户端请求，解析请求报文，由线程池中的工作线程调用
    void process();

    // 将新的客户数据初始化，放到数组中
    void init(int sockfd, const sockaddr_in &addr);

    // 关闭连接
    void close_conn();

    // 非阻塞一次性读完数据
    bool read();

    // 非阻塞一次性写数据
    bool write();

    // 解析HTTP请求
    // 主状态机状态
    HTTP_CODE process_read();

    HTTP_CODE parse_request_line(char *text);// 解析HTTP请求行

    HTTP_CODE parse_request_headers(char *text);// 解析HTTP请求头

    HTTP_CODE parse_request_content(char *text);// 解析HTTP请求体

    //从状态机状态，解析每一行
    LINE_STATUS parse_line();

    HTTP_CODE do_request();//解析获取具体的请求信息

    bool process_write( HTTP_CODE ret );  // 填充HTTP应答


    // 所有的socket上的事件都被注册到同一个epoll对象中
    static int m_epollfd;

    // 用户数量
    static int m_user_count;

    // 这一组函数被process_write调用以填充HTTP响应
    void unmap();// 对内存映射区执行munmap操作
    bool add_response(const char* format, ... );
    bool add_content(const char* content );
    bool add_content_type();
    bool add_status_line(int status, const char* title ); // title:状态码的描述
    bool add_headers(int content_length );
    bool add_content_length(int content_length );
    bool add_linger();
    bool add_blank_line(); // 添加空行

private:
    int m_sockfd;   // 该http连接的socket
    sockaddr_in m_address; //客户端通信的socke地址
    char m_read_buf[READ_BUFFER_SIZE];//读缓存区
    int m_read_idx; //标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx; //当前正在解析的字符在缓冲区的位置
    int m_start_line; //当前正在解析的行的起始位置
    CHECK_STATE m_check_state;//主状态机当前状态
    int m_content_length; // HTTP请求的消息总长度

    char *m_url;//请求目标文件的文件名
    char *m_version;//协议版本，此项目只支持HTTP1.1
    METHOD m_method;//请求方法，GET
    char *m_host;//主机名
    bool m_keepAlive;//HTTP请求是否保存连接

    void init();//初始化解析请求报文状态等相关信息

    // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char m_real_file[FILENAME_LEN];

    char *get_line() {//获得一行数据
        return m_read_buf + m_start_line;
    }

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

};





#endif
