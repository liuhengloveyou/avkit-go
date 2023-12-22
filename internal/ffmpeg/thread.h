#pragma once

#ifdef __EMSCRIPTEN_PTHREADS__
#include <pthread.h>
#else
#include <thread>
#endif

#include <atomic>
#include <thread>

class ThreadBase
{
public:
    ThreadBase();

    virtual void run() = 0;

    void stop();
    void start();

private:
#ifdef __EMSCRIPTEN_PTHREADS__
    pthread_t tid;
#else
    std::thread *m_th = nullptr;
#endif

protected:
    std::atomic<bool> m_stop; // = false;
};
