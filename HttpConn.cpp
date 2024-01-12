#include "HttpConn.h"

#include <fcntl.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* 定义HTTP响应的一些状态信息 */
const char *ok_200_title = "OK";
const char *error_400_title = "BAD REQUEST";
const char *error_400_form =
    "Your requesr has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Frobidden";
const char *error_403_form =
    "You do not have permission to get file from this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form =
    "The requested file was not found on this serber.\n";
const char *error_500_title = "Interal Error";
const char *error_500_form =
    "There was an unusual problem serving the requested file.\n";
/* 网站根目录 */
const char *doc_root = "/var/www/html";

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/* 静态变量类外初始化 */
int HttpConn::m_user_count = 0;
int HttpConn::m_epollfd = -1;

void HttpConn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void HttpConn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    /* 下面两行仅用于调试 */
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

void HttpConn::init() {
    m_check_state = CHECK_STATE::CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = METHOD::GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, 0, READ_BUFFER_SIZE);
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}

/* 从状态机 */
HttpConn::LINE_STATUS HttpConn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_STATUS::LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
            return LINE_STATUS::LINE_BAD;
        } else if (temp == '\n') {
            if ((m_checked_idx > 1) &&
                (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
            return LINE_STATUS::LINE_BAD;
        }
    }
    return LINE_STATUS::LINE_OPEN;
}

bool HttpConn::read() {
    if (m_read_idx < +READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                          READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        } else if (bytes_read == 0) {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

HttpConn::HTTP_CODE HttpConn::parse_request_line(char *text) {
    /* 请求行例如
     * GET /index.html HTTP/1.1
     * 所以m_url为第一个空格或者'\t'后面的内容
     */
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return HTTP_CODE::BAD_REQUEST;
    }
    *m_url++ = '\0';

    char *method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = METHOD::GET;
    } else {
        return HTTP_CODE::BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return HTTP_CODE::BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return HTTP_CODE::BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') {
        return HTTP_CODE::BAD_REQUEST;
    }

    m_check_state = CHECK_STATE::CHECK_STATE_HEADER;
    return HTTP_CODE::NO_REQUEST;
}

HttpConn::HTTP_CODE HttpConn::parse_headers(char *text) {
    if (text[0] == '\0') {
        if (m_method == METHOD::HEAD) {
            return HTTP_CODE::GET_REQUEST;
        }

        if (m_content_length != 0) {
            m_check_state = CHECK_STATE::CHECK_STATE_CONTENT;
            return HTTP_CODE::NO_REQUEST;
        }

        return HTTP_CODE::GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknow header %s\n", text);
    }

    return HTTP_CODE::NO_REQUEST;
}
