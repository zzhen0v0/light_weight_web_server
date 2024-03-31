#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

/*
基于生产者/消费者模型实现的一个线程安全的队列
主要就是两个接口 
1 push 
2 pop 相当于消费者 必须要有东西在队列才能pop

*/

#include <cassert>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <sys/time.h>
//#include "../lock/locker.h"
using namespace std;

template <typename T> 
class block_queue {
public:
    explicit block_queue(size_t max_capacity=1000) { }

    ~block_queue(){}

    void clear(){}

    void close(){}

    bool full(){}

    bool empty(){}
    
    size_t size() {}

    size_t capacity(){}

    T front(){}

    T back() {}

    void push_back(const T&){}

    void push_front(const T&){}

//把pop的item返回
    bool pop(T &item){}

    bool pop(T& item,int ms_timeout) {}

    void flush() {
        cond_consumer.notify_one();
    }
private:
    deque<T> dq;
    bool is_close;
    size_t _capacity;
    
    std::mutex mutex_dq;
    std::condition_variable cond_producer;
    std::condition_variable cond_consumer;
};
template <typename T>
block_queue<T>:: block_queue(size_t max_capacity=1000):_capacity(max_capacity) {
    assert(max_capacity>0)
    is_close=false;
}


template <typename T>
block_queue<T>:: ~block_queue(){
    close();
}

template <typename T>
void block_queue<T>::clear(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    dq.clear();
}

template <typename T>
void block_queue<T>:: close(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    is_close=true;
    dq.clear();
    cond_consumer.notify_all();
    cond_producer.notify_all();
}

template <typename T>
bool block_queue<T>::  full(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    return dq.size>=_capacity;
}

template <typename T>
bool block_queue<T>::empty(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    return dq.empty();
}

template <typename T>
size_t block_queue<T>:: size() {
    std::lock_guard<std::mutex> locker(mutex_dq);
    return dq.size();
}

template <typename T>
size_t block_queue<T>:: capacity(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    return _capacity;
}

template <typename T>
T block_queue<T>:: front(){
    std::lock_guard<std::mutex> locker(mutex_dq);
    return dq.front();
}

template <typename T>
T block_queue<T>:: back() {
    std::lock_guard<std::mutex> locker(mutex_dq);
    return dq.back();
}

template <typename T>
void block_queue<T>:: push_back(const T& item){
    std::lock_guard<std::mutex> locker(mutex_dq);
    while(dq.size()>=_capacity) {
        cond_producer.wait(locker);
    }
    dq.push_back(item);
    cond_consumer.notify_one();
}

template <typename T>
void block_queue<T>:: push_front(const T& item){
    std::lock_guard<std::mutex> locker(mutex_dq);
    while(dq.size()>=_capacity) {
        cond_producer.wait(locker);
    }
    dq.push_front(item);
    cond_consumer.notify_one();
}

template <typename T>
bool block_queue<T>:: pop(T &item){
    std::lock_guard<std::mutex> locker(mutex_dq);
    while(dq.empty()) {
        cond_consumer.wait(locker);
        if(is_close==false) {
            return false;
        }
    }
    item=dq.front();
    dq.pop_front();
    cond_producer.notify_one();
    return true;
}

template <typename T>
bool block_queue<T>:: pop(T &item,int ms_timeout) {}

#endif