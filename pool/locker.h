#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore>
#include <semaphore.h>


// 1.用于线程同步封装类：互斥锁类
class Locker {
public:
    // 构造函数
    Locker()
    {
        // 初始化互斥量
        if(pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            // 创建失败抛出异常
            throw std::exception();
        }
    }

    // 析构函数
    ~Locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    // 上锁
    bool lock()
    {
        // pthread_mutex_lock返回0说明拿到锁
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    // 解锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    // 获得互斥量成员
    pthread_mutex_t* getMutex()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; // 锁，互斥量
};




// 2.用于线程同步封装类：条件变量类
class Cond {
public:
    Cond()
    {
        // 初始化
        if(pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }

    ~Cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    // 一直等待，调用了该函数，线程会阻塞。阻塞时会自动解锁，解除阻塞时会自动拿锁
    bool wait(pthread_mutex_t *mutex)
    {
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    // 等待多长时间，调用了这个函数，线程会阻塞，直到指定的时间结束。时间到后解除阻塞继续向下运行
    bool timedwait(pthread_mutex_t *mutex, struct timespec t)
    {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    // 唤醒一个或者多个等待的线程
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    // 唤醒所有的等待的线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }


private:
    pthread_cond_t m_cond;
};



// 3.用于线程同步封装类：信号量类
class Sem {
public:
    Sem()
    {
        if(sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }

    // 创建指定数量的信号量
    Sem(int num)
    {
        if(sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }

    ~Sem()
    {
        sem_destroy(&m_sem);
    }

    // 对信号量加锁，调用一次对信号量的值-1，如果值为0，就阻塞
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }

    // 对信号量解锁，调用一次对信号量的值+1
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};



#endif
