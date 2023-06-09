#include <Base/currentThread.h>
#include <Base/exception.h>
#include <Base/thread.h>
#include <sys/prctl.h>    // prctl
#include <sys/syscall.h>  // SYS_gettid
#include <unistd.h>       // syscall getpid

namespace Lute {

namespace detail {
    /// @brief 该系统调用的返回值作为线程 ID
    /// 优点：
    ///   1. 类型是 pid_t，通常是小整数，便于在日志中输出
    ///   2. 现代 Linux 中，直接表示内核的任务调度ID，
    ///      因此在 /proc 文件系统中可以轻易找到对应项：
    ///         /proc/tid or /proc/pid/task/tid
    ///   3. 在其他系统工具中也容易定位到具体某一个线程
    ///   4. 任何时刻都是全局唯一的,并且由于Linux分配新pid采用递增轮回办法,
    ///       短时间内启动的多个线程也会具有不同的线程id
    ///   5. 0是非法值,因为操作系统第一个进程init的pid是1
    /// @return
    pid_t gettid() {
#if __unix__
        return static_cast<pid_t>(::syscall(SYS_gettid));
#else
#include <thread>
        return std::this_thread::get_id();
#endif
    }

    /// @brief 解决问题：万一程序执行了fork(2),那么子进程会不会看到
    /// stale 的缓存结果？ 利用 pthread_atfork
    /// 注册一个回调函数，用于清空缓存的线程ID
    void afterFork() {
        Lute::CurrentThread::t_cachedTid = 0;
        Lute::CurrentThread::t_threadName = "main";

        Lute::CurrentThread::tid();

        // no need to call pthread_atfork(NULL, NULL, &afterFork);
    }

    /// @brief Call afterFork
    class ThreadNameInitializer {
    public:
        ThreadNameInitializer() {
            Lute::CurrentThread::t_threadName = "main";
            Lute::CurrentThread::tid();
            pthread_atfork(nullptr, nullptr, &afterFork);
        }
    };

    ThreadNameInitializer init;

    /**
     * @brief func_ / name_ / tid_ / latch_
     */
    struct ThreadData {
        using ThreadFunc = Thread::ThreadFunc;

        ThreadFunc func_;
        std::string name_;
        pid_t* tid_;
        Lute::CountDownLatch* latch_;

        /// @brief Constructor
        /// @param func
        /// @param name
        /// @param tid
        /// @param latch
        ThreadData(ThreadFunc func, std::string name, pid_t* tid,
                   Lute::CountDownLatch* latch)
            : func_(std::move(func)),
              name_(std::move(name)),
              tid_(tid),
              latch_(latch) {}

        /// @brief Call this->func_()
        void runThread() {
            *tid_ = Lute::CurrentThread::tid();
            tid_ = nullptr;
            latch_->countDown();
            latch_ = nullptr;

            Lute::CurrentThread::t_threadName =
                name_.empty() ? "LuteThread" : name_.c_str();
            ::prctl(PR_SET_NAME, Lute::CurrentThread::t_threadName);

            try {
                func_();
                Lute::CurrentThread::t_threadName = "finished";
            } catch (const Lute::Exception& ex) {
                Lute::CurrentThread::t_threadName = "crashed";
                fprintf(stderr, "exception caught in Thread %s\n",
                        name_.c_str());
                fprintf(stderr, "reason: %s\n", ex.what());
                fprintf(stderr, "stack trace: %s\n", ex.stackTrace());
                abort();
            } catch (const std::exception& ex) {
                Lute::CurrentThread::t_threadName = "crashed";
                fprintf(stderr, "exception caught in Thread %s\n",
                        name_.c_str());
                fprintf(stderr, "reason: %s\n", ex.what());
                abort();
            } catch (...) {
                Lute::CurrentThread::t_threadName = "crashed";
                fprintf(stderr, "unknown exception caught in Thread %s\n",
                        name_.c_str());
                throw;  // rethrow
            }
        }
    };

    /// @brief Call runThread()
    /// @param arg ThreadData*
    /// @return nullptr
    void* startThread(void* arg) {
        auto* data = static_cast<ThreadData*>(arg);
        data->runThread();
        delete data;
        return nullptr;
    }
}  // namespace detail

/// @brief 是用__thread变量来缓 gettid(2) 的返回值 \n
/// 这样只有在本线程第一次调用的时候才进行系统调用,以后都是直接从thread
/// local缓存的线程id拿到结果
void CurrentThread::cacheTid() {
    if (t_cachedTid == 0) {
        t_cachedTid = detail::gettid();
        t_tidStringLength =
            snprintf(t_tidString, sizeof(t_tidString), "%5d ", t_cachedTid);
    }
}

/// @brief
/// @return return tid() == ::getpid()
bool CurrentThread::isMainThread() { return tid() == ::getpid(); }

/// @brief Sleep for @c usec
/// @param usec
void CurrentThread::sleepUsec(int64_t usec) {
    struct timespec ts = {0, 0};
    const int64_t kMicroSecondsPerSecond = 1E6;  // 1000 * 1000;
    ts.tv_sec = static_cast<time_t>(usec / kMicroSecondsPerSecond);
    ts.tv_nsec = static_cast<long>(usec % kMicroSecondsPerSecond * 1000);

    ::nanosleep(&ts, nullptr);
}
// ------------------------------------------------------------

// ------------------------------------------------------------
std::atomic_int32_t Thread::numCreated_;

Thread::Thread(ThreadFunc func, std::string name)
    : started_(false),
      joined_(false),
      pthreadId_(0),
      tid_(0),
      func_(std::move(func)),
      name_(std::move(name)),
      latch_(1) {
    setDefaultName();
}

Thread::~Thread() {
    if (started_ && !joined_) {
        pthread_detach(pthreadId_);
    }
}

/// @brief Set the name of thread to "Thread..."
void Thread::setDefaultName() {
    // int num = numCreated_.incrementAndGet();
    int num = numCreated_.fetch_add(1);
    if (name_.empty()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Thread%d", num);
        name_ = buf;
    }
}

/// @brief Thread start including:
/// 1. pthread_create()
/// 2. startThread()
/// 3. latch_.wait()
void Thread::start() {
    // Ensure the thread is not started
    assert(!started_);
    started_ = true;

    // FIXME: move(func_)
    auto* data = new detail::ThreadData(func_, name_, &tid_, &latch_);
    // auto* data =
    //     new detail::ThreadData(std::move(func_), name_, &tid_, &latch_);

    // 当线程创建时，线程函数就已经开始执行
    if (pthread_create(&pthreadId_, nullptr, &detail::startThread, data)) {
        started_ = false;
        delete data;  // or no delete?
        // LOG_SYSFATAL << "Failed in pthread_create";
        perror("Failed in pthread_create");
        abort();
    } else {
        latch_.wait();
        assert(tid_ > 0);
    }
}

/// @brief thread join
/// @return pthread_join(pthreadId_, nullptr)
int Thread::join() {
    // Ensure thread is started and not joined
    assert(started_);
    assert(!joined_);
    joined_ = true;

    return pthread_join(pthreadId_, nullptr);
}

}  // namespace Lute
