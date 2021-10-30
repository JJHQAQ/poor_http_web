#include "http_conn.h"

//定义一些HTTP响应的状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "you o not have permisson to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//网站的根目录
const char* doc_root = "http";

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;//EPOLLRDHUP对端断开链接
    if(one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    printf("addfd！fd:[%d]\n",fd);
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);

}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if(real_close && (m_sockfd != -1))
    {
        printf("closed fd == [%d]", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in & addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉,也就是端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, sockfd, true);
    ++m_user_count;
    init();
}

void http_conn::init()
{
    m_checked_state = CHECK_STATE_REQUESTION;
    m_linger = false;
    
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_real_file, '\0', sizeof(m_real_file));
    memset(m_read_buf, '\0', sizeof(m_read_buf));
    memset(m_write_buf, '\0', sizeof(m_write_buf));

}

//从状态机，用于解析出一行内容
http_conn::LINE_STATUS http_conn::parse_line()
{
    printf("pares_line()\n");
    char temp;

    for( ; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
        //获得当前分析的字
        temp = m_read_buf[m_checked_idx];
        //如果当前的字节是\r，即回车符，则说明可能读取到一个完整的行
        if(temp == '\r')
        {
            //如果\r字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有读取到一个完整的行，需要继续读取数据
            if(m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }    
            
            //表示读到了一个完整的行
            else if(m_read_buf[m_checked_idx + 1] == '\n')
            {
                printf("读到完整的行 m_checked_idx == [%d]\n", m_checked_idx);
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            printf("pares_line()语法错误\n");
            //以上都不是，就是存在语法错误
            return LINE_BAD;

        }
        //当前字符为\n也有可能是到了一行的情况
        else if(temp == '\n')
        {
            //因为\r\n一起用，还得判断
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                printf("读到完整的行 m_checked_idx == [%d]\n", m_checked_idx);
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            printf("pares_line() error\n");
            return LINE_BAD;
        }
        
    }
    //如果所有的字符都读完了还没有遇到
    return LINE_OPEN;

}

//循环读取客户端数据，知道无数据可读或者对方关闭链接
bool http_conn::read()
{
    printf("read()\n");
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//解析HTTP请求行，获取请求方法，目标URL以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char * text)
{
    m_url = strpbrk(text, " \t");
    if(!m_url)
    {
        printf("[-----%d-----]BAD_REQUEST[m_url]\n",m_sockfd);
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    
    char * method = text;
    if(strcasecmp(method,"GET") == 0)
    {
        printf("[-----%d-----]m_method get!\n",m_sockfd);
        m_method = GET;
    }
    else
    {
        printf("[-----%d-----]BAD_REQUEST[m_method]\n",m_sockfd);
        return BAD_REQUEST;
    }
    m_url += strspn(m_url," \t");
    m_version = strpbrk(m_url," \t");
    if(!m_version)
    {
        printf("[-----%d-----]BAD_REQUEST[m_version]\n",m_sockfd);
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    
    printf("[-----%d-----]m_version get!\n",m_sockfd);
    printf("%s\n",m_version);
    
    if(strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        printf("m_version:\n");
        printf("%s\n",m_version);
        printf("BAD_REQUEST[m_version is not http1.1]\n");
        return BAD_REQUEST;
    }
    if(strncasecmp(m_url,"http://",7) == 0)
    {
        printf("BAD_REQUEST[m_url is not http:]\n");
        m_url += 7;
        m_url = strchr(m_url, '/');
        printf("m_url get!\n");
    }
    if(!m_url || m_url[0] != '/')
    {
        printf("BAD_REQUEST[m_url和m_url[0]不一致]\n");
        return BAD_REQUEST;
    }
    m_checked_state = CHECK_STATE_HEADER;//状态改为需要读取头部行
    return NO_REQUEST;
}

//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char * text)
{
    printf("开始解析！剩余行数:[%d]",m_content_length);
    //遇到空行表示头部字段解析完毕
    if(text[0] == '\0')
    {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0)
        {
            m_checked_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    else if(strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text," \t");
        if(strcasecmp(text,"keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    //处理Content-length头部字段
    else if(strncasecmp(text, "Content-Length", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if(strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

//我们没有真正解析HTTP请求的消息体，只是判断他是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char * text)
{
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机process 1 retrun
http_conn::HTTP_CODE http_conn::process_read()
{
    printf("process_read()\n");
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char * text = 0;
    //
    while( ( (m_checked_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK) ) || ( (line_status = parse_line()) == LINE_OK))
    {
        printf("process_read while()!\n");
        text = get_line();
        m_start_line = m_checked_idx;//这里因为前面调用了parse_line导致计数器已经更新了
        printf("got 1 http line:%s\n",text);
        switch (m_checked_state)
        {
            case CHECK_STATE_REQUESTION:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                {
                    printf("parse_request_line BAD_REQUEST\n");
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                {
                    printf("parse_headers BAD_REQUEST\n");
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
                
            
            default:
            {
                return INTERNAL_ERROR;
            }     
        }
    }
    printf("process_read return\n");
    return NO_REQUEST;
}

//当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，对所有用户可读，且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者文件获取成功
http_conn::HTTP_CODE http_conn::do_request()
{
    printf("do_reaquest!\n");
    strcpy(m_real_file,doc_root);
    int len  = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    /*
    头文件：#include <sys/stat.h>   #include <unistd.h>

    定义函数：int stat(const char * file_name, struct stat *buf);

    函数说明：stat()用来将参数file_name 所指的文件状态, 复制到参数buf 所指的结构中。
    */
    printf("%s",m_real_file);
    if(stat(m_real_file, &m_file_stat) < 0)
    {
        printf("文件不存在！\n");
        return NO_REQUEST;
    }
    if(!(m_file_stat.st_mode & S_IROTH))
    {
        printf("文件不可读！\n");
        return FORBIDDEN_REQUEST;
    }
    if( S_ISDIR(m_file_stat.st_mode))
    {
        
        printf("文件是目录！\n");
        return BAD_REQUEST;
    }
    int fd = open(m_real_file, O_RDONLY);
    /*
    #inlcude<sys/mann.h>
    void mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void *start, size_t length);
    
    void *start 允许用户使用某一个人特定的地址为有这段内存的起始位置。如果他被设置为NULL，则系统自动分配一个地址。
    size_t length 此参数制定了内存段的长度
    int prot 此参数设置内存段访问权限：
            PROT_READ:可读
            PROT_WRITE:可写
            PROT_EXEC:可执行
            PROT_NONE:内存段不能被访问
    int flags 此参数控制内存段内容被修改后程序的行为。它可以被设置为以下值的按位或（MAP_SHARED和MAP_PRIVATE是互斥的，不能同时指定）
            MAP_SHARED:在进程间共享这段内存。对该内存段的修改将反应到被映射的文件中。它提供了进程间共享内存的POSIX方法
            MAP_PRIVATE:内存段调用为进程私有，对该内存段的修改不会反应到被映射的文件中
            MAP_ANONYMOUS:这段内存不是从文件映射而来的，其内容被初始化为全0，这种情况下，mmap函数的最后两个参数将被忽略
            MAP_FIXED:内存段必须位于start参数指定的地址处。start必须是内存页面大小（4096）的整数倍
            MAP_HUGETLB:按照大内存页面来分配内存空间。大内存页面的大小可以通过/pro/meminfo文件来查看
    int fd 此参数是被映射文件对应的文件描述符。他一般通过open系统调用获得。
    off_t offset此参数设置从文件的何处开始映射（对于不需要读入整个文件的情况）

    mmap函数成功时返回指向目标内存区域的指针，失败则返回MAO_FAILED((void*)-1)并设置errno

    munmap函数成功返回0.失败返回-1并设置errno
    
    */
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);//映射共享内存
    close(fd);
    return FILE_REQUEST;

}

//因为上面函数我们已经将打开的文件映射到了内存，所以必须对内存映射区执行munmap操作
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写http响应
bool http_conn::write()
{
    printf("[-----%d-----]wirte()\n",m_sockfd);
    int temp = 0;

    
    if(bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(1)
    {
        // printf("wirte() ready!\n");
        // temp = writev(m_sockfd, m_iv, m_iv_count);
        // printf("wirte() over!\n");
        // if(temp <= -1)
        // {
        //     //如果TCP写缓冲没有空间，则等待下一轮RPOLLOUT事件。虽然在此期间，服务器无法立即接受到同一个客户的下一个请求，但这可以保证链接的完整性。
        //     if(errno == EAGAIN)
        //     {
        //         printf("wirte() file for space lack!\n");
        //         modfd(m_epollfd, m_sockfd, EPOLLOUT);
        //         return true;
        //     }
        //     unmap();
        //     return false;
        // }

        // printf("wirte() fished!\n");
        // bytes_to_send -= temp;
        // bytes_have_send += temp;
        // if(bytes_to_send <= bytes_have_send)
        // {
        //     //发送HTTP相应成功，更具HTTP请求中的Connection字段决定是否立即关闭连接
        //     unmap();
        //     if(m_linger)
        //     {
        //         printf("长链接保持监听！\n");
        //         init();
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return true;
        //     }
        //     else
        //     {
        //         printf("短链接关闭监听！\n");
        //         modfd(m_epollfd, m_sockfd, EPOLLIN);
        //         return false;
        //     }
        // }

        //以下为了解决大文件传输问题 bycdy 20210708
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        //读了个temp字节数的文件
        bytes_have_send += temp;
        //已发送temp字节数的文件
        bytes_to_send -= temp;
        //如果可以发送的字节大于报头，证明报头发送完毕，但还有文件要发送
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //报头长度清零
            m_iv[0].iov_len = 0;
            /*这行代码：因为m_write_idx表示为待发送文件的定位点，m_iv[0]指向m_write_buf，
            所以bytes_have_send（已发送的数据量） - m_write_idx（已发送完的报头中的数据量）
            就等于剩余发送文件映射区的起始位置*/
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //否则继续发送报头，修改m_iv指向写缓冲区的位置以及待发送的长度以便下次接着发
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            //发送完毕，恢复默认值以便下次继续传输文件
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//往缓冲区写入待发送的数据,也就是写入了m_write_buf
bool http_conn::add_response(const char* format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);//接收可变参数
    int len = vsnprintf(m_write_buf + m_write_idx, 
                        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);//将可变参数写到第一个参数的内存里，写的长度为第二个参数

    if(len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);//释放可变参数资源
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}


//应答行的头部字段
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger == true)?"keep-alive":"close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char * content)
{
    return add_response("%s", content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    printf("process_write()\n");
    switch (ret)
    {
        case INTERNAL_ERROR:
        {
            add_status_line(500,error_500_title);
            add_status_line(500,error_500_form);
            if(!add_content(error_500_form))
            {
                return false;
            }
            break;
        }
        case BAD_REQUEST: 
        {
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
            {
                return false;
            }
            break;
        }
        case NO_REQUEST:
        {
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST://请求文件
        {
            add_status_line(200, ok_200_title);
            if(m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                printf("%s", m_write_buf);
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;//还需传入的数据字节
                return true;
            }
            else
            {
                const char * ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                {
                    return false;
                }
            }
            break;
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


//由线程池的工作调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        printf("process_read ret == NO_REQUEST! \n");
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    printf("process_write ready!");
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        printf("write_ret = false closed \n");
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

