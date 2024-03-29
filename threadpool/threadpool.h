#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;            //线程池中的线程数
    int m_max_requests;             //请求队列中允许的最大请求数
    pthread_t *m_threads;           //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;     //请求队列
    locker m_queuelocker;           //保护请求队列的互斥锁
    sem m_queuestat;                //是否有任务需要处理
    connection_pool *m_connPool;    //数据库
    int m_actor_model;              //模型切换
};

template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number]; //这里pthread_t是unsigned_int，代表线程id   
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))   //分离
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
//析构函数
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); //信号量+1
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)  //返回任意类型的指针的函数,arg是传进来的this指针
{
    threadpool *pool = (threadpool *)arg;   //?
    pool->run();
    return pool;
}


template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();         //信号量为0时，程序会阻塞在这里，除非有其他线程把信号量+1；否则信号量--，往下执行
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        if (1 == m_actor_model) //Reactor
        {
            if (0 == request->m_state)  //读模式
            {
                if (request->read_once())
                {
                    request->improv = 1;    //证明读事件完成了，以正确事件结束
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;        //证明读事件完成了，但是是以错误方式结束，此时应该断开连接（证明客户端断开了连接或出现问题）
                    request->timer_flag = 1;    //是否要销毁这个timer的标志
                }
            }
            else                        //写模式
            {
                if (request->write())   
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else                    //Proactor 已经由主线程读过了，不需要再读了
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
