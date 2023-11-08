#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include "httpConn.h"
#include "locker.h"


// 线程池类
// 定义成模板类，方便后面代码的复用
// 模板参数T在本项目中为任务类
// 模板定义声明最好在一个文件，不然可能会报错

template<typename T>
class ThreadPool {
public:
    // 线程数量：8, 最大请求数量：10000
    ThreadPool(int thread_num = 8, int max_requests = 10000);

    ~ThreadPool();

    // 向请求队列添加任务
    bool append(T* request);


private:
    // 静态函数，创建线程时使用
    static void *worker(void *arg);

    // 子线程创建后的工作函数
    void run();



private:
    // 线程池初始化的线程数量
    int m_thread_number;

    // 线程池数组容器，大小为线程池初始化的线程数量m_thread_number
    pthread_t* m_threads;

    // 请求队列中最大允许的等待处理的线程数量
    int m_max_requests;

    // 用于装任务的请求队列
    std::list< T*> m_workQueue;

    // 互斥锁
    Locker m_queueLocker;

    // 信号量，判断是否有任务需要处理
    Sem m_queuestat;

    // 是否结束线程
    bool m_stop;
};



// 实现

// 构造函数
template<typename T>
ThreadPool<T>::ThreadPool(int thread_num, int max_requests):
    m_thread_number(thread_num), m_max_requests(max_requests),
    m_stop(false), m_threads(NULL)
{
    if(thread_num <= 0 || max_requests <= 0)
    {
        throw std::exception();
    }

    // 初始化线程池数组容器，线程池线程数量m_thread_number
    m_threads = new pthread_t[m_thread_number];

    // 如果创建失败
    if(!m_threads)
    {
        throw std::exception();
    }

    // 创建m_thread_number个线程，并将他们设置成线程分离，让线程自己释放资源
    for (int i = 0; i < m_thread_number; i++)
    {
        printf("Create the num: %d thread\n", i);

        // 作为this指针参数传入线程执行函数worker，此函数必须为静态函数
        // 后面work函数即可访问非静态成员对象
        if(pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            // 如果创建失败
            delete []m_threads;
            throw std::exception();
        }

        // 将他们设置成线程分离
        if(pthread_detach(m_threads[i]) != 0)
        {
            // 如果分离失败
            delete []m_threads;
            throw std::exception();
        }
    }
}



// 析构函数
template<typename T>
ThreadPool<T>::~ThreadPool()
{
    delete []m_threads;
    m_stop = true;
}



// 向请求队列添加任务
template<typename T>
bool ThreadPool<T>::append(T* request)
{
    // 上锁
    m_queueLocker.lock();

    // 如果超出最大允许的等待处理的线程数量
    if(m_workQueue.size() > m_max_requests) {
        m_queueLocker.unlock();
        return false;
    }

    m_workQueue.push_back(request);

    // 解锁
    m_queueLocker.unlock();

    // 信号量增加，方便后续根据信号量数量控制阻塞与非阻塞
    // 信号量大于0说明请求队列有任务
    m_queuestat.post();

    return true;
}


// 静态函数，创建线程时使用
template<typename T>
void *ThreadPool<T>::worker(void *arg)
{
    ThreadPool *pool = (ThreadPool *) arg;

    // 让线程池运行起来
    pool->run();

    return pool;
}


// 子线程创建后的工作函数
template<typename T>
void ThreadPool<T>::run()
{
    // 各个线程都在运行此函数，谁先获取到请求队列中的请求，谁先执行process函数，执行完后继续等待
    // 一直循环运行等待获取请求队列中的请求进行处理
    while(!m_stop)
    {
        // 对信号量加锁，调用一次对信号量的值-1，如果值为0，就阻塞
        // 大于0说明有请求队列有任务
        m_queuestat.wait();

        // 上锁
        m_queueLocker.lock();

        // 如果请求队列为空，此时还没有客户端连接，各个子线程一直运行到此循环
        if(m_workQueue.empty())
        {
            m_queueLocker.unlock();
            continue;
        }

        // 取请求队列第一个请求
        T* request = m_workQueue.front();

        // 取请求队列数据后删除第一个数据
        m_workQueue.pop_front();

        // 解锁
        m_queueLocker.unlock();

        // 如果没有取到数据
        if(!request)
        {
            continue;
        }

        // 如果该子线程取到了数据，对请求报文进行处理
        // 同步模仿Proactor模式
        request->process();

    }
}


#endif
