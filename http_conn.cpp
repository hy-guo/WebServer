#include "http_conn.h"

const char *resources_root_path = "/resource"; // Web资源目录

const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the requested file.\n";

int http_conn::m_epoll_fd;
int http_conn::m_user_count;

void add_fd(int epoll_fd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);

    int opt = fcntl(fd, F_GETFL) | O_NONBLOCK;
    fcntl(fd, F_SETFL, opt);
}

void remove_fd(int epoll_fd, int fd)
{
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modify_fd(int epoll_fd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLRDHUP | EPOLLONESHOT;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
}

// 关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        remove_fd(m_epoll_fd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;

        in_addr client_ip;
        client_ip.s_addr = m_address.sin_addr.s_addr;
        printf("Disconnection: %s:%d\tConnection Num: %d\n", inet_ntoa(client_ip), ntohs(m_address.sin_port), m_user_count);
    }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int socket_fd, const sockaddr_in &client_addr)
{
    m_sockfd = socket_fd;
    m_address = client_addr;

    // 端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    add_fd(m_epoll_fd, socket_fd);
    m_user_count++;

    in_addr client_ip;
    client_ip.s_addr = m_address.sin_addr.s_addr;
    printf("New Connection: %s:%d\tConnection Num: %d\n", inet_ntoa(client_ip), ntohs(m_address.sin_port), m_user_count);

    reset();
}

// 重置HTTP状态
void http_conn::reset()
{
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_method = GET;                          // 默认请求方式为GET

    m_url = 0;
    m_version = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_headers.clear();
    m_content = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, MAX_FILENAME_LEN);
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0); // 接收数据到读缓冲区
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) // 没有数据
            {
                break;
            }
            return false;
        }
        else if (bytes_read == 0) // 连接关闭
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;

        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
                return do_request();
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

// 解析一行
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r'))
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text; // HTTP方法
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else
        return BAD_REQUEST;

    m_version = strpbrk(m_url, " \t");
    if (!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0'; // HTTP版本
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0)
        m_url += 7;

    m_url = strchr(m_url, '/');
    if (!m_url || strlen(m_url) == 1)
    {
        strcpy(m_url, "/index.html");
    }
    else if (m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查请求头
    return NO_REQUEST;
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 空行，请求头解析完毕
    if (text[0] == '\0')
    {
        if (m_headers.find("Content-Length") != m_headers.end() && stoi(m_headers["Content-Length"]) > 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else
    {
        char *value = strpbrk(text, ":");
        *value++ = '\0';
        m_headers[text] = ++value;
    }
    return NO_REQUEST;
}

// 解析请求体
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (stoi(m_headers["Content-Length"]) + m_checked_idx))
    {
        text[stoi(m_headers["Content-Length"])] = '\0';
        m_content = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    int len = 0;
    strcpy(m_real_file, get_current_dir_name());
    len += strlen(m_real_file);
    strncpy(m_real_file + len, resources_root_path, MAX_FILENAME_LEN - len - 1);
    len += strlen(resources_root_path);
    strncpy(m_real_file + len, m_url, MAX_FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0) // 获取m_real_file文件的相关的状态信息
    {
        return NO_RESOURCE;
    }

    if (!(m_file_stat.st_mode & S_IROTH)) // 判断访问权限
    {
        return FORBIDDEN_REQUEST;
    }

    if (S_ISDIR(m_file_stat.st_mode)) // 判断是否是目录
    {
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); // 创建内存映射
    close(fd);
    return FILE_REQUEST;
}

// 解除内存映射
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 发送HTTP响应
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modify_fd(m_epoll_fd, m_sockfd, EPOLLIN);
        reset();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count); // 分散写入数据
        if (temp <= -1)
        {
            if (errno == EAGAIN) // TCP写缓冲满
            {
                modify_fd(m_epoll_fd, m_sockfd, EPOLLOUT);
                return true;
            }
            else
            {
                unmap();
                return false;
            }
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        // 修改下一轮开始发送的位置
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) // 数据发送完毕
        {
            unmap();
            modify_fd(m_epoll_fd, m_sockfd, EPOLLIN);

            if (m_headers.find("Connection") != m_headers.end() && m_headers["Connection"] == "keep-alive")
            {
                reset();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

// 往写缓冲中写入一条待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

// 写入响应行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//写入响应头
bool http_conn::add_headers(int content_len)
{
    add_response("Content-Length: %d\r\n", content_len);
    add_response("Content-Type:%s\r\n", "text/html");
    add_response("Connection: %s\r\n", (m_headers.find("Connection") != m_headers.end()) ? m_headers["Connection"] : "close");
    add_response("%s", "\r\n");
    return true;
}

// 生成HTTP应答，并写入写缓冲区
bool http_conn::process_write(HTTP_CODE request_stat)
{
    switch (request_stat)
    {
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        add_headers(m_file_stat.st_size);
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        bytes_to_send = m_write_idx + m_file_stat.st_size;
        return true;
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        add_response("%s", error_500_form);
        break;
    case BAD_REQUEST:
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        add_response("%s", error_400_form);
        break;
    case NO_RESOURCE:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        add_response("%s", error_404_form);
        break;
    case FORBIDDEN_REQUEST:
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        add_response("%s", error_403_form);
        break;
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modify_fd(m_epoll_fd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modify_fd(m_epoll_fd, m_sockfd, EPOLLOUT);
}