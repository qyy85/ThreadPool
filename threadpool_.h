#pragma once
#include<iostream>
#include<vector>
#include<queue>
#include<memory>
#include<mutex>
#include<atomic>
#include<condition_variable>
#include<functional>
#include<unordered_map>
#include<future>
#include<thread>
const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 50;
const int THREAD_MAX_FREE_TIME = 5;

//枚举类型
enum PoolMode{
    MODE_FIXED,
    MODE_CACHED
};
class Thread{
public:
    using ThreadFunc = std::function<void(int)>;
    Thread(ThreadFunc func){
         this->func_ = func;
        threadId_ = generateId_++;
    }
    ~Thread()=default;
    //启动线程
    void start(){
        //创建线程，执行线程函数
        std::thread t(func_, threadId_);
        //分离线程
        t.detach();
    }
    //获取线程ID
    size_t getId(){
       return threadId_;     
    }
private:
    //线程函数对象包装类型
    ThreadFunc func_;
    static size_t generateId_;  
    //线程ID
    size_t threadId_; 
};
size_t Thread::generateId_= 0;

class ThreadPool{
public:
        ThreadPool():initThreadSize_(0), 
                    taskSize_(0), 
                    taskQMaxThreshHold_(TASK_MAX_THRESHHOLD), 
                    poolMode_(PoolMode::MODE_FIXED),
                    isPoolRunning_(false),
                    freeThreadSize_(0),
                    threadSzieThreshhold_(THREAD_MAX_THRESHHOLD),
                    curThreadSize_(0){}
        ~ThreadPool(){
            isPoolRunning_=false;
            //等待线程池中所有线程返回
            //两种状态
            //notEmpty_阻塞、正在工作
            std::unique_lock<std::mutex> lock(taskQMutex_);
            //针对第二种情况
            notEmpty_.notify_all();
            //阻塞主线程
            exitCond_.wait(lock, [&](){return threads_.size()==0;});
        }
        
        ThreadPool(const ThreadPool&)=delete;
        ThreadPool& operator=(const ThreadPool&)=delete;
        //设置线程池的工作模式
        void setMode(PoolMode mode){
            if (checkRunningState())
			    return;
		        poolMode_ = mode;
            
        }
    
        //设置任务队列的上线阈值
        void setTaskQMaxThreshHold(size_t threshhold){
            if(checkRunningState())
                return;
            taskQMaxThreshHold_ = threshhold;
        }    
        //设置线程池cached模式下，线程的个数的上线阈值
        void setThreadSzieThreshhold(int threshhold){
            if(checkRunningState())
                return;
            if(poolMode_==PoolMode::MODE_CACHED)
                threadSzieThreshhold_ = threshhold;
        }

        //开启线程池
        void start(size_t initThreadSize = std::thread::hardware_concurrency()){
             //线程池的运行状态开启
            isPoolRunning_ = true;
            //线程的初始个数
            initThreadSize_ = initThreadSize;
            //当前线程个数
            curThreadSize_ = initThreadSize;
            //创建初始线程对象
            for(int i=0; i<initThreadSize_;i++){
                //使用emplace_back，比push_back要好，都是尾插
                // std::unique_ptr<Thread> ptr(new Thread(std::bind(&ThreadPool::threadFunc, this)));
                auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
                size_t threadid = ptr->getId();
                threads_.emplace(threadid, std::move(ptr));
            }
            //启动线程
            for(int i=0; i<initThreadSize_;i++){
                //使用emplace_back，比push_back要好，都是尾插
                threads_[i]->start();
                freeThreadSize_++;
            }
        }
         
         //提交任务
         //使用可变参模板编程，可以提交任意任务函数和任意数量参数
         //使用decltype(func())可以推导函数的返回值类型，函数有参数需要加上参数
         //再把推导出来的类型，给future类指定类型，用future存储返回值
        template<typename Func, typename... Types>
        auto submitTask(Func&& func, Types&&...args) -> std::future<decltype(func(args...))>{
            //获取返回值类型
            using RType = decltype(func(args...));
            // 打包任务
            // 1、使用共享智能指针管理packaged_task类型对象，packaged_task类型对象怎么用，指针对象就怎么用
            // 2、packaged_task类型包装绑定器
            // 3、bind绑定器绑定任务函数
            // 具体就是共享智能指针管理一个返回值类型是rType的packaged_task对象
            // packaged_task对象初始为绑定器绑定相同返回值类型的函数
            // 函数及其参数使用forward保证类型不变
            auto task = std::make_shared
            <std::packaged_task<RType()>>
            (std::bind(std::forward<Func>(func), std::forward<Types>(args)...));
            std::future<RType> result = task->get_future();
            //获取锁
            std::unique_lock<std::mutex> uniquelock(taskQMutex_);
            //线程通信，等待任务队列有空余
            //设置等待时间，最长不能阻塞1s,否则判断提交任务失败，返回
            if(!notFull_.wait_for(uniquelock, std::chrono::seconds(1), 
                [&]()->bool{return taskQueue_.size()<taskQMaxThreshHold_;}))
            {
               //表示等待1s后，任不能满足条件
                std::cout<<"task queue if full, submit task fail..."<<std::endl;
                auto task = std::make_shared<std::packaged_task<RType()>>([]()->RType{ return RType();});
                (*task)();
                return task->get_future();
            }
            //添加任务
            // using Task = std::function<void(void)>;
            // std::queue<Task> taskQueue_; 
            //不是使用task.get()获取原始地址
            //使用lambda表达式按值进行捕获
            taskQueue_.emplace([task](){(*task)();});
            taskSize_++;

            //添加任务，任务队列不空， 唤醒任务线程获取任务
            notEmpty_.notify_all();

            //cached模式，任务处理比较急的，小而快的任务
            //根据任务的数量和空闲线程的数量，判断释放需要创建新的线程
            if(poolMode_==PoolMode::MODE_CACHED && taskSize_>freeThreadSize_ && curThreadSize_<threadSzieThreshhold_){
                //创建新的线程对象
                auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
                size_t threadId = ptr->getId();
                threads_.emplace(threadId, std::move(ptr));
                threads_[threadId]->start();
                //当前线程个数
                curThreadSize_++;
                //空闲线程个数
                freeThreadSize_++;
                //打印一样，主线程调用
                std::cout<<"create new thread: "<<std::endl;     
            }
            return result;
        }                                     
private:
        //线程执行函数
        void threadFunc(int threadId){
            auto lastTime = std::chrono::high_resolution_clock().now();
            //设计任务线程必须执行完所有任务在退出
            while (true)
            {
               Task task;
                {
                    //获取锁
                    std::unique_lock<std::mutex> uniquelock(taskQMutex_);
                    //cached模式下，创建的线程，超时60s空闲的线程，应该回收
                    //当然剩下的线程数量最少为initThreadSize_
                    //注意条件判断，设置双重判断，isPoolRunning_==true && ==0，没有任务进入循环将线程阻塞，大于0直接获取任务
                    while (taskQueue_.size()==0){ 
                        //判断线程池是否关闭
                        if(!isPoolRunning_){
                            threads_.erase(threadId);
                            std::cout<<"thread Id:"<<std::this_thread::get_id()<<" exit."<<std::endl; 
                            exitCond_.notify_all();
                            return;
                        }
                    if(poolMode_==PoolMode::MODE_CACHED){ 
                            //超时返回了
                            if(std::cv_status::timeout == notEmpty_.wait_for(uniquelock, std::chrono::seconds(1))){
                                auto nowTime = std::chrono::high_resolution_clock().now();
                                auto time_ = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
                                if(time_.count()>=THREAD_MAX_FREE_TIME && curThreadSize_>initThreadSize_){
                                    //回收当前线程
                                    //记录线程相关数量的改变
                                    //把线程从线程队列中删除
                                    //使用线程ID删除
                                    threads_.erase(threadId);
                                    curThreadSize_--;
                                    freeThreadSize_--;
                                    std::cout<<"thread Id:"<<std::this_thread::get_id()<<" exit."<<std::endl;
                                    return; 
                                }
                            }   
                        }else{
                            notEmpty_.wait(uniquelock);
                        }
                    }
                    //取出一个任务
                    std::cout<<"线程ID:"<<std::this_thread::get_id()<<" 获取任务"<<std::endl;
                    task = taskQueue_.front();
                    taskQueue_.pop(); 
                    freeThreadSize_--;
                    taskSize_--;
                    //如果还要任务，通知其他线程
                    if(taskQueue_.size()>0){
                        notEmpty_.notify_all();
                    }
                    notFull_.notify_all();
                } //作用域结束，释放锁
                if(task!=nullptr){
                    //直接取出任务执行
                    task();
                }
                freeThreadSize_++; 
                lastTime = std::chrono::high_resolution_clock().now();
            }
        }
        //检查线程池的运行状态
        bool checkRunningState() const{
            return isPoolRunning_;
        }

        // std::vector<std::unique_ptr<Thread>> threads_;     //线程列表
        std::unordered_map<size_t, std::unique_ptr<Thread>> threads_;
        size_t initThreadSize_;                               //初始线程数量
        size_t threadSzieThreshhold_;                      //线程的个数上线阈值
        std::atomic_uint freeThreadSize_;                  //空闲线程的数量   
        std::atomic_uint curThreadSize_;                   //记录当前线程的总数       

        //将任务队列中的任务使用函数对象包装器包装
        using Task = std::function<void(void)>;
        std::queue<Task> taskQueue_;                      //任务队列
        std::atomic_uint taskSize_;                       //任务数量
        size_t taskQMaxThreshHold_;                       //任务队列上线阈值
        std::mutex taskQMutex_;                           //任务队列互斥锁
        std::condition_variable notFull_;                 //线程通信，表示任务队列不满
        std::condition_variable notEmpty_;                //线程通信，宝石任务队列不空
        std::condition_variable exitCond_;                //等待线程全部回收

        PoolMode poolMode_;                               //当前线程的模式
        std::atomic_bool isPoolRunning_;                   //线程池的运行状态
};
