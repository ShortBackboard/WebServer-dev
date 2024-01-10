#include "include/httpConn.h"

// 网站的工作目录
const char *doc_root = "/home/cnu/WebServer-dev/resources";
// ./server 10000
// http://192.168.56.101:10000/index.html
// sudo chmod 777 index.html 修改访问权限


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";


// 设置某个文件描述符非阻塞
void setNonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}


// 添加监听的文件描述符相关的检测信息到epoll中
void addfd(int epollfd, int fd, bool one_shot) {
    // event:保存要监听的事件和fd
    struct epoll_event event;

    event.data.fd = fd;

    // 要监听的事件
    // EPOLLRDHUP事件：底层解决连接双方一方突然断开的错误处理
    event.events = EPOLLIN | EPOLLRDHUP;


    // oneshot指的某socket对应的fd事件最多只能被检测一次，不论你设置的是读写还是异常。
    // 因为可能存在这种情况：如果epoll检测到了读事件，数据读完交给一个子线程去处理
    // 如果该线程处理的很慢，在此期间epoll在该socket上又检测到了读事件，
    // 则又给了另一个线程去处理，则在同一时间会存在两个工作线程操作同一个socket。
    // EPOLLONESHOT:防止同一个通信被不同的线程处理
    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }

    // EPOLL_CTL_ADD：追加
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setNonblocking(fd);
}

// 从epoll中删除文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，注意重置EPOLLONESHOT事件，确保下次可读时，EPOLLIN事件触发
void modfd(int epollfd, int fd, int ev) {
    struct epoll_event event;

    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int setnonblocking( int fd ) {
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}


// 对静态变量初始化
int HttpConn::m_epollfd = -1;
int HttpConn::m_user_count = 0;


// 非阻塞一次性读完数据
bool HttpConn::read() {
    printf("read client data\n");
    if(m_read_idx >= READ_BUFFER_SIZE) return false;

    // 读取到的字节
    int bytes_read = 0;

    // 循环一次性读完数据
    while(1){
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, sizeof(m_read_buf) - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                // 表示非阻塞读取数据完毕，没有数据了，不是错误
                break;
            }
            return false;
        }else if(bytes_read == 0) {
            // 客户端关闭连接
            return false;
        }

        m_read_idx += bytes_read;
    }
    printf("\n读取到的数据:%s\n", m_read_buf);
    return true;
}

// 非阻塞一次性写HTTP响应
bool HttpConn::write() {
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数

    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_keepAlive) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            }
        }
    }
}

//初始化解析请求报文状态等相关信息，私有方法
void HttpConn::init(){
    m_check_state = CHECK_STATE_REQUESTLINE;//初始化状态为解析请求行
    m_checked_idx = 0;//当前正在解析的字符在缓冲区的位置
    m_start_line = 0; //当前正在解析的行的起始位置
    m_read_idx = 0;//标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个位置

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_keepAlive =  false;

    // 把读缓冲区清空
    bzero(m_read_buf, READ_BUFFER_SIZE);

    m_start_line = 0;
    m_write_idx = 0;


    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}


// 解析HTTP请求，主状态机的状态
HTTP_CODE HttpConn::process_read(){
    // 初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    // 获取的一行数据
    char* text = 0;

    // 解析到了请求体或者是解析到了一行完整的数据
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {

        // 获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        printf( "got 1 http line: %s\n", text );


        // 有限状态机以及转换，依次读取请求报文
        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_request_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {
                    return do_request();//解析具体的请求信息
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_request_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: { //其余
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;
}

// 解析HTTP请求行，获得请求方法，目标URL，HTTP版本
// 参数text:要解析的一行数据
// todo:用正则表达式解析
HTTP_CODE HttpConn::parse_request_line(char *text){
    // 1.GET /index.html HTTP/1.1

    // strpbrk:依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，
    // 则停止检验，并返回该字符位置。
    // \t:'/'
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) {
        return BAD_REQUEST;
    }

    // 2.GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符

    char  *method = text;//\0：结束符，GET\0/index.html HTTP/1.1，method=GET;

    // strcasecmp:两个字符串比较，忽略大小写
    if ( strcasecmp(method, "GET") == 0 ) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    // 3./index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }

    // 4./index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }


    // eg:http://192.168.110.129:10000/index.html

    if (strncasecmp(m_url, "http://", 7) == 0 ) {
        m_url += 7;//192.168.110.129:10000/index.html
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST;

}

// 解析HTTP请求头信息
HTTP_CODE HttpConn::parse_request_headers(char *text){
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_keepAlive = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 解析HTTP请求体
// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
HTTP_CODE HttpConn::parse_request_content(char *text){
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//解析每一行，根据请求报文的报文格式的尾部\r\n，回车符换行符判断
LINE_STATUS HttpConn::parse_line(){
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {
            // xx\r\nxx
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {
                return LINE_OPEN;
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {
                m_read_buf[ m_checked_idx++ ] = '\0';//把\r\n变成\0结束符
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            // xx\r
            // \nxx
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                m_read_buf[ m_checked_idx-1 ] = '\0';
                m_read_buf[ m_checked_idx++ ] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析获取具体的请求信息
// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
HTTP_CODE HttpConn::do_request()
{
    // "doc_root：/home/cnu/WebServer-dev/resources"
    strcpy( m_real_file, doc_root );
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );

    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
// 释放资源
void HttpConn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}



// 处理客户端请求，解析请求报文，由线程池中的工作线程调用
// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void HttpConn::process() {
    // 解析http请求
    // 有限状态机：按照\n切换不同的状态，解析请求行、请求头部、请求空行、请求体；
    // 不同的状态执行不同的业务逻辑

    // 服务器处理HTTP请求的可能结果，报文解析的结果
    HTTP_CODE read_ret = process_read();

    if(read_ret == NO_REQUEST) {
        // NO_REQUEST: 请求不完整，需要继续读取客户数据
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;//表示此函数的结束
    }

    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }

    // 生成响应后，将该通信的socketfd事件改为EPOLLOUT
    // 主线程中epoll监听到此事件就把响应报文写给客户端
    modfd( m_epollfd, m_sockfd, EPOLLOUT);


}


// 将新的客户数据初始化，放到数组中
void HttpConn::init(int sockfd, const sockaddr_in &addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    int reuse{1};
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;// 总用户数加1

    //初始化解析请求报文状态等相关信息
    init();
}


// 关闭一个客户端连接
void HttpConn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}


// 往写缓冲中写入待发送的数据
bool HttpConn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool HttpConn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool HttpConn::add_headers(int content_len) {
    return add_content_length(content_len) & add_content_type() &
        add_linger() & add_blank_line();
}

bool HttpConn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool HttpConn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_keepAlive == true ) ? "keep-alive" : "close" );
}

bool HttpConn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool HttpConn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool HttpConn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool HttpConn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}
