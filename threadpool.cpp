#include<iostream>
#include"head/threadpool.h"
#include<thread>
#include<mutex>
#include<chrono>
const int TASK_MAX_THRESHHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_FREE_TIME = 5;
//线程构造
ThreadPool::ThreadPool()
:initThreadSize_(0), 
taskSize_(0), 
taskQMaxThreshHold_(TASK_MAX_THRESHHOLD), 
poolMode_(PoolMode::MODE_FIXED),
isPoolRunning_(false),
freeThreadSize_(0),
threadSzieThreshhold_(THREAD_MAX_THRESHHOLD),
curThreadSize_(0){}

//线程池析构
ThreadPool::~ThreadPool(){
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
//设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode){
    if(checkRunningState())
        return;
        poolMode_ = mode;
}

//设置任务队列的上线阈值
void ThreadPool::setTaskQMaxThreshHold(size_t threshhold){
    if(checkRunningState())
        return;
    taskQMaxThreshHold_ = threshhold;
}  
//设置线程池cached模式下，线程的个数的上线阈值
void ThreadPool::setThreadSzieThreshhold(int threshhold){
    if(checkRunningState())
        return;
    if(poolMode_==PoolMode::MODE_CACHED)
        threadSzieThreshhold_ = threshhold;
}
//开启线程池
void ThreadPool::start(size_t initThreadSize){
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
Result ThreadPool::submitTask(std::shared_ptr<Task> task){
    //获取锁
    std::unique_lock<std::mutex> uniquelock(taskQMutex_);
    //线程通信，等待任务队列有空余
    //设置等待时间，最长不能阻塞1s,否则判断提交任务失败，返回
    if(!notFull_.wait_for(uniquelock, std::chrono::seconds(1), 
        [&]()->bool{return taskQueue_.size()<taskQMaxThreshHold_;}))
    {
        //表示等待1s后，任不能满足条件
        std::cout<<"task queue if full, submit task fail..."<<std::endl;
        //一般返回值采用两种方法
        //1、return task->getResult();
        //2、return Result(task);
        //在这里采用第二种， task指针，在线程执行完后，会被析构，这时依赖task去调用返回Result对象是不合理的
        //可以采用右值引用，将即将销毁的task赋值给Result的成员，同时返回Result对象，是较好的
        return Result(task, false);
    }
    //添加任务
    taskQueue_.emplace(task);
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
    return Result(task);
}
//线程执行函数
void ThreadPool::threadFunc(int threadId){
    auto lastTime = std::chrono::high_resolution_clock().now();
    //设计任务线程必须执行完所有任务在退出
    while (true)
    {
        std::shared_ptr<Task> task;
        {
            //获取锁
            std::unique_lock<std::mutex> uniquelock(taskQMutex_);
            std::cout<<"线程ID:"<<std::this_thread::get_id()<<" 获取任务"<<std::endl;
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

                // if(!isPoolRunning_){
                //     threads_.erase(threadId);
                //     std::cout<<"thread Id:"<<std::this_thread::get_id()<<" exit."<<std::endl;
                //     //线程退出，通知exitCond_条件变量阻塞的线程，检查条件是否满足
                //     exitCond_.notify_all();
                //     return;
                // }
            }
            //取出一个任务
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
            // task->run();
            //在下面的基类普通方法中调用子类run方法，并实现setVal保存task任务的返回值
            task->execute();
         }
        freeThreadSize_++; 
        lastTime = std::chrono::high_resolution_clock().now();
    }
}
//检查线程池的运行状态
bool ThreadPool::checkRunningState() const{
    return isPoolRunning_;
}

///////Task类的相关方法////////
Task::Task(){
    result_ = nullptr;
}
Task::~Task(){}

void Task::execute(){
    if(result_!=nullptr)
        result_->setVal(run());
}
void Task::setResult(Result* res){
    result_ = res;
}

///////Thread类的方法实现///////
size_t Thread::generateId_= 0;
Thread::Thread(ThreadFunc func){
    this->func_ = func;
    threadId_ = generateId_++;
}
size_t Thread::getId(){
    return threadId_;
}
Thread::~Thread(){

}
//启动线程
void Thread::start(){
    //创建线程，执行线程函数
    std::thread t(func_, threadId_);
    //分离线程
    t.detach();
}

//Result返回值类型
 Result::Result(std::shared_ptr<Task> task, bool isVaild){
     task_ = task;  
     isVaild_ = isVaild;
     task->setResult(this);   
 }

Any Result::get(){
    if(!isVaild_){
        return NULL;
    }
    sem_.wait();
    return std::move(any_);
}

void Result::setVal(Any any){
    //存储task返回值
    any_ = std::move(any);
    //获取任务返回值，增加信号量资源
    sem_.post();
}

Result::~Result(){

}