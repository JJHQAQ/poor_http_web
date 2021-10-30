#ifndef HTTP_CONN_H
#define HTTP_CONN_H

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

class http_conn
{
public:
    
    static const int FILENAME_LEN = 200;//文件名的最大长度
    
    static const int READ_BUFFER_SIZE = 2048;//缓冲区的大小
    
    static const int WRITE_BUFFER_SIZE = 1024;//写缓冲区的大小


    //HTTP请求方法
    //HTTP请求方法,但本代码中仅仅支持GET
    enum METHOD{
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH
    };


    //主状态机可能的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTION = 0,//正在分析当前请求行
        CHECK_STATE_HEADER,//正在分析头部字段
        CHECK_STATE_CONTENT
    };

    //从状态机可能的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,//读取到一个完整的行
        LINE_BAD,//行出错
        LINE_OPEN//行数据尚且不完整
    };

    //服务器处理http请求的结果
    enum HTTP_CODE
    {
        NO_REQUEST,//请求不完整需要继续读取
        GET_REQUEST,//得到了一个完整的请求
        BAD_REQUEST,//请求有语法错误
        NO_RESOURCE,//没有资源
        FORBIDDEN_REQUEST,//没有足够的权限
        FILE_REQUEST,//文件已被请求
        INTERNAL_ERROR,//服务器内部错误
        CLOSED_CONNECTION//客户端连接已关闭
    };

public:
    http_conn(){}
    ~http_conn(){}

public:
    
    void init(int sockfd, const sockaddr_in& addr);//初始化新的链接
   
    void close_conn(bool real_close = true); //关闭链接
    
    void process();//处理客户请求
    
    bool read();//非阻塞读操作
    
    bool write();//非阻塞写操作

private:
   
    void init(); //初始化链接
    
    HTTP_CODE process_read();//解析http请求
    
    bool process_write(HTTP_CODE ret);//填充http应答

    //下面这一组函数被process调用以分析HTTP请求
    HTTP_CODE parse_request_line(char * text);
    HTTP_CODE parse_headers(char * text);
    HTTP_CODE parse_content(char * text);
    HTTP_CODE do_request();
    char * get_line()
    {
        return m_read_buf + m_start_line;
    }
    LINE_STATUS parse_line();

    //下面这一组函数被process调用以填充HTTP应答
    void unmap();//清空内存映射区，
    bool add_response(const char* format,...);//往缓冲区写入待发送的数据
    bool add_content(const char * content);//
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_lenth);
    bool add_linger();
    bool add_blank_line();

public:
    
    static int m_epollfd;//所有socket上的时间都被注册到同一个epoll内核事件表中，所以将epoll文件描述符设置为静态的
    
    static int m_user_count;//统计用户数量


private:
    
    int m_sockfd;//该HTTP链接的socket和对方的socket地址

    struct sockaddr_in m_address;

    
    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    
    int m_read_idx;//标识度缓冲区已经读入的客户数据的最后一个字节的下一个位置
    
    int m_checked_idx;//当前正在分析的字符在读缓冲区的位置
    
    int m_start_line;//当前正在解析的行的起始位置
   
    char m_write_buf[WRITE_BUFFER_SIZE]; //写缓冲区
  
    int m_write_idx;  //写缓冲区待发送的字节

    
    CHECK_STATE m_checked_state;//主状态机当前所处的状态
   


    METHOD m_method; //请求方法

    char m_real_file[FILENAME_LEN];//客户请求目标文件的完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
   
    char * m_url; //客户请求的目标文件的文件名
    
    char * m_version;//HTTP协议版本号，我们仅支持HTTP1.1
    
    char * m_host;//主机名
    
    int m_content_length;//http请求的消息体的长度
    
    bool m_linger;//HTTP请求是否要求保持链接

    
    char * m_file_address;//客户请求的目标文件被mmap到内存中的起始位置
   
    struct stat m_file_stat; //目标文件的状态。通着这个我们可以判断木变文件是否存在，是否为目录，是否可读，并获得文件大小等信息
   
    struct iovec m_iv[2]; //我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示背写内存块的数量

    int m_iv_count;

    int bytes_to_send;//缓冲区待发送的字节
    int bytes_have_send;//缓冲区已经发送的字节

};


#endif
