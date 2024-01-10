#ifndef DLL_TIMER
#define DLL_TIMER

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include <unordered_map>

#include "../include/httpConn.h"

// 定时器类
class TimerNode
{
public:
    TimerNode() {}

public:
    time_t expire;                // 任务超时时间，这里使用绝对时间
    void (*cb_func)(HttpConn *); // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    HttpConn *user_data;
};

// 定时器小根堆
class sort_timer_srp
{
public:
    sort_timer_srp();
    // 链表被销毁时，删除其中所有的定时器
    ~sort_timer_srp();

    // 将目标定时器timer添加到链表中
    void add_timer(TimerNode *timer);

    /* 当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
    超时时间延长的情况，即该定时器需要往链表的尾部移动。*/
    void adjust_timer(TimerNode *timer);

    // 将目标定时器 timer 从链表中删除
    void del_timer(TimerNode *timer);

    /* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
    void tick();

    int getsize_() { return size_; }
    int getrefsize() { return ref_.size(); }

private:
    // 小根堆排序算法
    void swifdown_(int u);
    void swifup_(int u);
    void swapnode_(int a, int b);

private:
    int size_ = 0; // 目前的大小

    std::unordered_map<TimerNode *, int> ref_; // 保存定时器和对应的下标

    TimerNode **heap_; // 小根堆
};

#endif
