#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdio.h>
#include <pthread.h>
#include <list>
#include <exception>
#include <semaphore.h>

#define NUM_THREADS 16     // 默认线程数量
#define MAX_REQUESTS 60000 // 默认最大请求队列长度

// 线程池
template <typename T>
class thread_pool
{
public:
    thread_pool(int thread_number = NUM_THREADS, int max_requests = MAX_REQUESTS);
    ~thread_pool();
    bool append(T *request); // 添加任务到任务队列

private:
    int m_thread_number, m_max_requests;
    pthread_t *m_threads;        // 线程数组
    std::list<T *> m_task_queue; // 任务队列
    pthread_mutex_t m_task_queue_mutex;
    sem_t m_task_queue_sem;
    bool m_stop;

    static void *thread_func_static(void *arg);
    void thread_func();
};

template <typename T>
thread_pool<T>::thread_pool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false)
{
    if ((thread_number <= 0) || (max_requests <= 0))
    {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        throw std::exception();
    }
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, thread_func_static, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
    printf("Create %d threads successfully!\n", thread_number);

    if (pthread_mutex_init(&m_task_queue_mutex, NULL) != 0)
    {
        throw std::exception();
    }
    if (sem_init(&m_task_queue_sem, 0, 0) != 0)
    {
        throw std::exception();
    }
}

template <typename T>
thread_pool<T>::~thread_pool()
{
    m_stop = true;
    delete[] m_threads;
    pthread_mutex_destroy(&m_task_queue_mutex);
    sem_destroy(&m_task_queue_sem);
}

template <typename T>
bool thread_pool<T>::append(T *request)
{
    pthread_mutex_lock(&m_task_queue_mutex);
    if (m_task_queue.size() > m_max_requests)
    {
        pthread_mutex_unlock(&m_task_queue_mutex);
        return false;
    }
    m_task_queue.push_back(request);
    pthread_mutex_unlock(&m_task_queue_mutex);
    sem_post(&m_task_queue_sem);
    return true;
}

template <typename T>
void *thread_pool<T>::thread_func_static(void *arg)
{
    thread_pool *pool = (thread_pool *)arg;
    pool->thread_func();
    return pool;
}

template <typename T>
void thread_pool<T>::thread_func()
{

    while (!m_stop)
    {
        sem_wait(&m_task_queue_sem);
        pthread_mutex_lock(&m_task_queue_mutex);
        if (m_task_queue.empty())
        {
            pthread_mutex_unlock(&m_task_queue_mutex);
            continue;
        }
        T *request = m_task_queue.front();
        m_task_queue.pop_front();
        pthread_mutex_unlock(&m_task_queue_mutex);
        if (!request)
        {
            continue;
        }
        request->process();
    }
}

#endif