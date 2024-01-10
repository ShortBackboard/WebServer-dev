#include "srp_timer.h"

const int MAX_HEAP_SIZE = 65536;

sort_timer_srp::sort_timer_srp()
{
    size_ = 0;
    heap_ = new TimerNode*[MAX_HEAP_SIZE];
}

// 链表被销毁时，删除其中所有的定时器
sort_timer_srp::~sort_timer_srp() {
    size_ = 0;
    delete [] heap_;
    ref_.clear();
}

// 将目标定时器timer添加到链表中
void sort_timer_srp::add_timer(TimerNode *timer ) {
    printf("++++++++++++++++++++++++++++++\n");
    if( !timer ) {
        return;
    }
    heap_[++size_] = timer;
    ref_[timer] = size_;
    swifup_(size_);
}

void sort_timer_srp::adjust_timer(TimerNode *timer)
{
    if( !timer ){
        return;
    }
    int idx = ref_[timer];
    if(idx < 0)return;
    swifdown_(idx);
}

// 将目标定时器 timer 从链表中删除
void sort_timer_srp::del_timer( TimerNode* timer )
{
    printf("------------------------------\n");
    if( !timer ) {
        return;
    }
    int idx = ref_[timer];
    if(idx < 0)return;
    swapnode_(idx, size_);

    heap_[size_--] = nullptr;

    if(size_)swifup_(idx);
    if(size_)swifdown_(idx);

    ref_.erase(timer);

    delete timer;
}

/* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
void sort_timer_srp::tick() {
    if( size_ == 0 ) {
        return;
    }
    printf( "timer tick\n" );
    time_t cur = time( NULL );  // 获取当前系统时间
    TimerNode* tmp = heap_[1];
    // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
    while( size_ ) {
        /* 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，
        比较以判断定时器是否到期*/
        if( cur < tmp->expire ) {
            break;
        }

        // 调用定时器的回调函数，以执行定时任务
        tmp->cb_func( tmp->user_data );
        // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
        del_timer(tmp);
        if(size_ == 0) break;
        tmp = heap_[1];
    }
}


void sort_timer_srp::swifdown_(int u)
{
    // 当前节点与子节点相互比较
    int t=u;
    if(u*2<=size_&&(heap_[u*2]->expire<heap_[t]->expire))t=u*2;
    if(u*2+1<=size_&&(heap_[u*2+1]->expire<heap_[t]->expire))t=u*2+1;

    if(u!=t) // 如果不是最小的，继续递归
    {
        swapnode_(t,u);
        swifdown_(t);
    }
    return;
}

void sort_timer_srp::swifup_(int u)
{
    while(u/2&&(heap_[u/2]->expire > heap_[u]->expire)) // 保证父节点存在且当前节点小于父节点
    {
        swapnode_(u,u/2);
        u=u/2;
    }
}

void sort_timer_srp::swapnode_(int a, int b)
{
    std::swap(ref_[heap_[a]],ref_[heap_[b]]);
    std::swap(heap_[a],heap_[b]);
}
