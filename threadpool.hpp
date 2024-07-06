#pragma once
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <future>
#include <iostream>

const int MAX_IDLE_THREAAD_WAIT_TIME=10;
const int DEFAULT_SIZE=16;
const int THREAD_SIZE_THRESHOLD=1024;
const int TASK_SIZE_THRESHOLD=1;
const int SUBMIT_TASK_TIME_OUT=1;
enum class Mode{
    /*
     * 线程池支持fixed模式和cached模式
     *
     * MODE_FIXED:表示线程池采用fixed模式,即线程池中线程数量是在线程池启动之前就确定下来,后续不会动态改变
     *
     * MODE_CACHED:表示线程池采用cached模式,当线程池中待处理的任务数量大于现有线程个数时
     * 线程池可以动态创建线程并处理任务,当任务处理完毕后,可以将空闲线程进行回收,
     * 并保证线程池中线程数量始终大于初始设置的线程数量
     */
    MODE_FIXED,
    MODE_CACHED
};

class Thread{
    /*
     * 对c++11std::thread类进行封装
     *
     * std::thread类在创建对象时就会开启一个线程
     * 将创建线程的操作封装在start()方法中能更为灵活的控制线程的启动
     * 且为线程设置一个threadId 方便在cached模式下回收线程
     */
public:
    using ThreadFunc=std::function<void(int)>;
    Thread(ThreadFunc func)
            :threadId_(baseId_++),
             func_(func){}
    ~Thread()=default;
    void start(){
        std::thread t(func_,threadId_);
        t.detach();
    }
    int getId() const{
        return threadId_;
    }
private:
    static uint32_t baseId_;
    uint32_t threadId_;
    ThreadFunc func_;
};

//静态成员类外初始化
uint32_t Thread::baseId_=0;

class ThreadPool{
    /*
     * 线程池类
     */
public:

    ThreadPool(uint32_t size=DEFAULT_SIZE,Mode mode=Mode::MODE_FIXED,
               uint32_t threadSizeThreshold=THREAD_SIZE_THRESHOLD,uint32_t taskSizeThreashold=TASK_SIZE_THRESHOLD):
            threadSizeThreshold_(threadSizeThreshold),
            taskSizeThreshold_(taskSizeThreashold),
            taskCount_(0),
            mode_(mode),
            started_(false),
            idleThreadSize_(0),
            threadSize_(0){
        /*
         *
         */
        if(size>0){
            if(size<threadSizeThreshold_){
                initThreadSize_=size;
            }else{
                initThreadSize_=DEFAULT_SIZE;
            }
        }else{
            throw "threadpool create exception";
        }
    }
    ~ThreadPool(){
        started_= false;
        threads_.clear();
        std::unique_lock<std::mutex> lck(mtxQue_);
        threadsEmpty_.wait(lck,[&]()->bool{return taskCount_==0;});
    }
    void setInitThreadSize(size_t size){
        /*
         * 创建池对象时没有指定初始线程个数,线程池启动之前,设置初始线程数量
         */
        if(started_){
            std::cerr<<"threadPool has already start !"<<std::endl;
        }else{
            initThreadSize_=size;
        }

    }
    void setMaxThreadSize(size_t size){
        /*
         * 创建池对象时没有指定最大线程数量,线程池启动之前,设置最大线程数量
         */
        if(started_){
            std::cerr<<"threadPool has already start !"<<std::endl;
        }else{
            threadSizeThreshold_=size;
        }
    }
    void setTaskMaxThreshold(size_t size){
        /*
         * 创建池对象时没有指定任务队列最大长度,线程池启动之前,设置任务队列最大长度
         */
        if(started_){
            std::cerr<<"threadPool has already start !"<<std::endl;
        }else{
            taskSizeThreshold_=size;
        }
    }
    void setMode(Mode mode){
        /*
         * 创建池对象时没有指定线程池模式,线程池启动之前,设置线程池模式
         */
        if(started_){
            std::cerr<<"threadPool has already start !"<<std::endl;
        }else{
            mode_=mode;
        }
    }

    template<typename Func,typename... Args>
    auto submit(Func&& func,Args&&... args)-> std::future<decltype(func(args...))>{
        /*
         * 向线程池中提交任务,并获取任务的返回值
         * 如果如果任务队列已满,等待一个超时时间
         * 若在这段时间内,任务队列仍然为满,任务提交失败,向用户返回一个期待类型的空值
         *
         * 当线程池处于cached模式下
         * 任务数量大于空闲线程数量时，一次性创建出所需要的线程
         */
        using RType= decltype(func(args...));
        std::shared_ptr<std::packaged_task<RType()>> task=std::make_shared<std::packaged_task<RType()>>(std::bind(std::forward<Func>(func),std::forward<Args>(args)...));
        std::future<RType> res=task->get_future();
        std::unique_lock<std::mutex> lck(mtxQue_);
        if(!notFull_.wait_for(lck,std::chrono::seconds(SUBMIT_TASK_TIME_OUT),[&]()->bool{
            return taskCount_<taskSizeThreshold_;})){
            std::cerr<<"task submit fail"<<std::endl;
            std::packaged_task<RType()> failTask([]()->RType{ return RType();});
            failTask();
            return failTask.get_future();
        }
        taskCount_++;
        tasksQue_.emplace([task]()->void{(*task)();});

        if(mode_==Mode::MODE_CACHED&&idleThreadSize_<taskCount_){
            for (int i = 0; i < std::min(taskCount_.load()-idleThreadSize_.load(),threadSizeThreshold_-threadSize_.load()); ++i) {
                auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
                int threadId=ptr->getId();
                threads_.emplace(threadId,std::move(ptr));
                threads_[threadId]->start();
                idleThreadSize_++;
                threadSize_++;
            }
        }
        //出作用域释放锁 那就晚点notify 避免线程过早苏醒
        notEmpty_.notify_all();
        return res;
    }
    void start(){
        /*
         * 创建线程,启动线程
         */
        started_=true;
        for(int i=0;i<initThreadSize_;++i){
            auto ptr=std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc,this,std::placeholders::_1));
            threads_.emplace(ptr->getId(),std::move(ptr));
            threadSize_++;
        }
        for (int i = 0; i < initThreadSize_; i++){
            threads_[i]->start();
            idleThreadSize_++;
        }
    }
private:
    void threadFunc(int threadIdInPool){
        /*
         * 线程函数:从任务队列中获取任务 若获取不到任务将线程阻塞等待唤醒
         * 如果工作在cached模式下 还要将超过空闲时间的线程回收并且保持池中线程数量>=iniThreadSize(初始线程数量)
         * 当线程池关闭(析构时)负责将线程池中的剩余任务完成,并且回收线程资源
         */
        size_t timestamp;
        if(mode_==Mode::MODE_CACHED){
            timestamp=getCurrentSecond();
        }
        while(true){//线程得一直执行任务
            TaskFunc task;
            {
                std::unique_lock<std::mutex> lck(mtxQue_);
                while (taskCount_==0){
                    if(mode_==Mode::MODE_CACHED){
                        if(std::cv_status::timeout==notEmpty_.wait_for(lck,std::chrono::seconds(1))){
                            if(!started_){
                                if(taskCount_==0){
                                    threadSize_--;
                                    idleThreadSize_--;
                                    threadsEmpty_.notify_all();
                                    return;
                                }
                            }
                            size_t currentTime=getCurrentSecond();
                            if(currentTime-timestamp>=MAX_IDLE_THREAAD_WAIT_TIME&&threadSize_>initThreadSize_){
                                threads_.erase(threadIdInPool);
                                threadSize_--;
                                idleThreadSize_--;
                                return;
                            }
                        }
                    }else{
                        if(std::cv_status::timeout==notEmpty_.wait_for(lck,std::chrono::seconds(1))){
                            if(!started_){
                                threadSize_--;
                                idleThreadSize_--;
                                if(threadSize_==0){
                                    threadsEmpty_.notify_all();
                                }
                                return;
                            }
                        }
                    }
                }
                idleThreadSize_--;
                taskCount_--;
                task=tasksQue_.front();
                tasksQue_.pop();
                if(taskCount_>0){
                    notEmpty_.notify_all();
                }
                notFull_.notify_all();
            }
            if(task){
                task();
                idleThreadSize_++;
            }
            if(!started_){
                if(taskCount_==0){
                    threadSize_--;
                    idleThreadSize_--;
                    threadsEmpty_.notify_all();
                    return;
                }
            }
            if(mode_==Mode::MODE_CACHED){
                timestamp=getCurrentSecond();
            }
        }
    }
    int getCurrentSecond(){
        /*
         * 用于获得系统当前时间 单位:秒
         */
        auto now=std::chrono::system_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
        return static_cast<int>(duration.count());
    }
private:
    //线程池模式
    Mode mode_;
    //线程池状态
    std::atomic_bool started_;
    //管理线程池创建的线程
    std::unordered_map<int,std::unique_ptr<Thread>> threads_;
    //保证线程池析构时所有线程均退出(且任务都执行完毕)的条件变量
    std::condition_variable threadsEmpty_;
    //线程池初始线程数量 fixed模式:池中线程数量  cached模式:池中线程数量下限
    uint32_t initThreadSize_;
    //线程池中实时实际线程数量
    std::atomic_uint32_t threadSize_;
    //专用于cached模式下 创建线程数量的上限
    uint32_t threadSizeThreshold_;
    //池中空闲线程数量
    std::atomic_uint32_t idleThreadSize_;

    using TaskFunc=std::function<void()>;
    //任务队列
    std::queue<TaskFunc> tasksQue_;
    //待处理的任务数量
    std::atomic_uint32_t taskCount_;
    //任务数量最大值
    uint32_t taskSizeThreshold_;
    //任务队列的互斥锁
    std::mutex mtxQue_;
    //任务队列中有任务时 唤醒线程执行任务
    std::condition_variable notEmpty_;
    //任务队列不满时用于唤醒线程提交任务
    std::condition_variable notFull_;

};
