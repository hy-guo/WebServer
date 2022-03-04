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
#include <string>
#include <map>
#include <unordered_map>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>

#define MAX_FILENAME_LEN 200   // 文件名的最大长度
#define READ_BUFFER_SIZE 2048  // 读缓冲区的大小
#define WRITE_BUFFER_SIZE 2048 // 写缓冲区的大小

class http_conn
{
public:
    static int m_epoll_fd;   // epoll描述符
    static int m_user_count; // 用户数

    http_conn() {}
    ~http_conn() {}

    void init(int sockfd, const sockaddr_in &addr); // 初始化新接受的连接
    void close_conn();                              // 关闭连接
    void process();                                 // 处理客户端请求
    bool read();                                    // 接受数据
    bool write();                                   // 发送数据

private:
    enum HTTP_REQUEST // HTTP请求
    {
        GET,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT
    };

    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE, //正在分析请求行
        CHECK_STATE_HEADER,      //正在分析头部字段
        CHECK_STATE_CONTENT      //正在解析请求体
    };

    enum HTTP_CODE
    {
        NO_REQUEST,        //请求不完整
        GET_REQUEST,       //获得完整请求
        BAD_REQUEST,       //错误请求
        NO_RESOURCE,       //没有资源
        FORBIDDEN_REQUEST, //无权限
        FILE_REQUEST,      //成功获取文件
        INTERNAL_ERROR,    //内部错误
        CLOSED_CONNECTION  //关闭连接
    };

    enum LINE_STATUS
    {
        LINE_OK,  //获得完整行
        LINE_BAD, //出错
        LINE_OPEN //行不完整
    };

    void reset();                      // 重置连接状态
    HTTP_CODE process_read();          // 解析HTTP请求
    bool process_write(HTTP_CODE ret); // 将HTTP响应写入写缓冲区

    // 读
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request(); // 将请求的文件映射到内存

    // 写
    void unmap();
    bool add_response(const char *format, ...);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);

    int m_sockfd;          // 连接的socket
    sockaddr_in m_address; // 连接的地址

    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区
    int m_read_idx;                    // 已读入缓冲区的位置
    int m_checked_idx;                 // 解析到的位置
    int m_start_line;                  // 当前行的起始位置

    CHECK_STATE m_check_state; // 主状态机当前所处的状态

    HTTP_REQUEST m_method;                                  // 请求方法
    char *m_version;                                        // HTTP版本号
    char m_real_file[MAX_FILENAME_LEN];                     // 请求的文件路径
    char *m_url;                                            // 请求的文件名
    std::unordered_map<std::string, std::string> m_headers; // 请求头
    char *m_content;                                        // 请求体

    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int m_write_idx;                     // 写缓冲区已写入的字节数
    char *m_file_address;                // 目标文件映射的位置
    struct stat m_file_stat;             // 目标文件的状态
    struct iovec m_iv[2];                // 待发送数据，m_iv[0]为HTTP响应行与响应头，m_iv[1]为响应体
    int m_iv_count;                      // 带发送数据的数量

    int bytes_to_send;   // 将要发送的数据的字节数
    int bytes_have_send; // 已经发送的字节数
};

#endif