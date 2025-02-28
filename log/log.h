#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
#include "../lock/locker.h"

class Log {
public:
    static Log *get_instance() {
        static Log instance;
        return &instance;
    }

    //not
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    // 因为日志有多个level 为了方便起见 宏定义如下
    void write_log(int level, const char *format, ...);

    //not 
    void flush(void);


private:
    Log(){}
    virtual ~Log(){}
    //not
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    //日志文件属性
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    FILE *m_fp;         //打开log的文件指针
    int m_split_lines;  //日志最大行数
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    
    //日志缓冲区
    int m_log_buf_size; //日志缓冲区大小
    char *m_buf;

    //为了异步
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async;                  //是否同步标志位
    locker m_mutex;

};

// 因为日志有多个level 为了方便起见 宏定义如下
#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)

#endif