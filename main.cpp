#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/time_wheel_timer.h"

#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性
extern int addfd(int epollfd, int fd, bool one_shot);
extern int remove(int epollfd, int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
// 创建定时器容器链表
//static sort_timer_lst timer_lst;

// 创建定时器时间轮
static time_wheel timeWheel;


static int epollfd = 0;

//信号处理函数：仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    // 将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    // 设置信号捕捉函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
//    timer_lst.tick();
    timeWheel.tick();
    alarm(TIMESLOT);    // 触发 SIGALRM 信号
}

//定时器回调函数
void cb_func(client_data *user_data)
{
    // 删除非活动连接在sockfd上的注册事件，并关闭文件描述符
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}

void show_error(int connfd, const char *info)
{
    printf("%s", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char *argv[])
{
#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif

    if (argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN);

    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "Chenjunyi1998.", "yourdb", 3306, 8);
    //创建线程池
    threadpool<http_conn> *pool = NULL;
    pool = new threadpool<http_conn>(connPool);


    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    // 设置写端非阻塞：若不设置非阻塞，则若缓冲区满导致阻塞，会进一步增加信号处理函数的执行时间
    setnonblocking(pipefd[1]);
    // 设置读端为 ET 非阻塞
    addfd(epollfd, pipefd[0], false);

    // 设置 SIGALRM，时间到了会触发
    addsig(SIGALRM, sig_handler, false);
    // 设置 SIGTERM，kill 会触发
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    // 创建连接资源数组
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    int time_expire = 0;
    // 每隔 TIMESLOT 时间触发 SIGALRM 信号
    alarm(TIMESLOT);
    cout << "port is :" << port<< endl;
    while (!stop_server)
    {
        // 监测注册到 epollfd 事件表的事件，并将已经发生的事件复制到 events 中
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // 轮询文件描述符
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                // 该连接分配的文件描述符
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                // 创建定时器临时变量
//                util_timer *timer = new util_timer;
//                time_wheel_timer *timer = new time_wheel_timer;

                // 设置定时器对应的连接资源
//                timer->user_data = &users_timer[connfd];
//                timer->cb_func = cb_func;

//                time_t cur = time(NULL);
//                time_expire = cur + 3 * TIMESLOT;  // TIMESLOT = 5
                time_expire = 3 * TIMESLOT;  // TIMESLOT = 5
//                users_timer[connfd].timer = timer;
//                timer_lst.add_timer(timer);
                users_timer[connfd].timer = timeWheel.add_timer(time_expire);
                users_timer[connfd].timer->cb_func = cb_func;

#endif

#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0)
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    if (http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
//                    util_timer *timer = new util_timer;
//                    time_wheel_timer *timer = new time_wheel_timer;

//                    timer->user_data = &users_timer[connfd];
//                    timer->cb_func = cb_func;
//                    time_t cur = time(NULL);
//                    timer->expire = cur + 3 * TIMESLOT;
//                    time_expire = cur + 3 * TIMESLOT;
                    time_expire = 3 * TIMESLOT;

//                    users_timer[connfd].timer = timer;
//                    timer_lst.add_timer(timer);
                    users_timer[connfd].timer = timeWheel.add_timer(time_expire);
                    users_timer[connfd].timer->cb_func = cb_func;
                }
                continue;
#endif
            }

                // 处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                time_wheel_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if (timer)
                {
                    timeWheel.del_timer(timer);
                }
            }

                //处理信号
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];  // buffer
                // 将数据从管道读到缓冲区，如果读端无数据，则返回 -1
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)  // 一般不会为 0
                {
                    continue;
                }
                else // 正常情况下返回所读数据的长度，此处总是 1
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                            case SIGALRM:
                            {
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

                //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                // 创建定时器临时变量，将该连接对应的定时器取出来
                time_wheel_timer *timer = users_timer[sockfd].timer;
                // 通常使用 I/O 实现 Reactor，使用异步 I/O 实现 Proactor
                // 此项目使用同步 I/O 模拟 Proactor
                // 主线程读完成后【users[sockfd].read_once()】，选择一个工作线程来处理客户请求
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入线程池的请求队列
                    pool->append(users + sockfd);
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        timeWheel.del_timer(timer);
                        time_expire = 3 * TIMESLOT;
                        users_timer[sockfd].timer = timeWheel.add_timer(time_expire);
                        users_timer[sockfd].timer->cb_func = cb_func;
                    }

                }
                else
                {
                    // 服务器端关闭连接，移除对应的定时器
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timeWheel.del_timer(timer);
                    }
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                time_wheel_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        timeWheel.del_timer(timer);
                        time_expire = 3 * TIMESLOT;
                        users_timer[sockfd].timer = timeWheel.add_timer(time_expire);
                        users_timer[sockfd].timer->cb_func = cb_func;
                    }

                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timeWheel.del_timer(timer);
                    }
                }
            }
        }
        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 完成读写事件后，再进行处理
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
