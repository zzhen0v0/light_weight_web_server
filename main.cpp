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
#include <signal.h>
#include <time.h>
#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
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
#include <sys/wait.h>
#include <sys/uio.h>
#include <error.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"
#include "./http/http_conn.h"

#define MAX_FD 65536           //最大文件描述符 也就是最多的用户confd
#define MAX_EVENT_NUMBER 10000 //最大事件数 最多可监听的事件个数
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
static sort_timer_lst timer_lst;
static int epollfd = 0;

void sig_handler(int sig) {
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void addsig(int sig,void(handler)(int),bool restart=true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof sa);
    sa.sa_handler =handler;
    if(restart) 
        sa.sa_flags|=SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL)!=-1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数 也就是说连接超时我们做什么，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
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

int main(int argc,char argv[]) {

#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif
    if(argc<=1) {
        printf("usage: %s ip_address port_number\n",basename(argv[0]));
        return 1;
    }
    int port=atoi(argv[1]);
    
    //忽略SIG_PIPE 为了防止客户端宕机 服务端还继续写数据 现在由程序自主负责
    addsig(SIGPIPE,SIG_IGN);
    
    //创建数据库连接池
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "ZZ@zhuzhen618514", "yourdb", 3306, 8);

    //创建线程池
    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch (...)
    {
        return 1;
    }

    //存放每个链接的信息 用confd作为下标
    http_conn *users = new http_conn[MAX_FD];
    assert(users);

    //初始化数据库读取表
    users->initmysql_result(connPool);

    //创建监听sd
    int listenfd =socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd>=0);

    //设置监听sd属性
    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_port=htons(port);
    address.sin_family=AF_INET;
    address.sin_addr=htonl(INADDR_ANY);

    //复用端口 bind listen
    int flag=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sieof(flag));
    ret=bind(listenfd,(struct sockaddr*) &address,sizeof(address));
    assert(ret>=0);
    ret=listen(listenfd,5);
    assert(ret>=0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd, listenfd, false);
    //http_conn 也就是工作线程具体的任务类中的事件由同一个内核时间表监听
    http_conn::m_epollfd = epollfd;

    //创建管道 用于主线程和其他工作线程信号的交互
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //写端不能为阻塞
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    // 统一事件元 当主线程收到工作线程发来的信号  我们就用sig_handler处理
    // 往写端写入信号 epoll监听读端 我们仅能对信号到来事件进行处理
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    //存放每个链接的定时器 用confd作为下标
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while(!stop_server) {
        int number=epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        
        if(number<0&&error!=EINTR){
            LOG_ERROR("%s","epoll failure");
            break;
        }

        for(int i=0;i<number;i++) {
            int sockfd=events[i].data.fd;
            //新的链接请求
            if(sockfd==listenfd) {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,client_addrlength);
                if(connfd<0){
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                //如果达到最大可连接数 告诉客户端 满了 
                if (http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                //表示accept成功
                //初始化一个http_conn类 并保存该信息
                user[connfd].init(connfd,client_address);
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
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
                    util_timer *timer = new util_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            //如果客户端出错
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }

            //如果是工作线程发来了信号事件
            else if((sockfd==pipefd[0])&&(events[i].events&EPOLLIN)) {
                int sig;
                char signals[1024];
                ret=recv(pipefd[0],signals,sizeof(signals),0);
                if(ret==-1||ret==0){
                    continue;
                }
                //对所有接收到的信号处理
                else {
                    for(int i=0;i<ret;i++){
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout=true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server=true;
                        }
                        default:
                            break;
                        }
                    }
                }   
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                //取出当前连接的定时器
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read_once())
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
            }
            //如果工作线程处理完业务逻辑 要求写connfd
            else if (events[i].events & EPOLLOUT)
            {
                util_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        } 
        //如果主线程收到alarm信号 就要对定时容器执行一次查询 排除所有的非活动链接
        if(timeout){
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