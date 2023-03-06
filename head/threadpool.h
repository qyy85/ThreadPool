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
#include<thread>
 //基类
class Base{
    public:
        virtual ~Base() = default;
};
//派生类
template<typename T>
class Derive:public Base{
    public: 
        Derive(T data):data_(data){}
        T data_;
};
//Any类型，可以接受任意数据的类型
class Any{
public:
    Any()=default;
    ~Any()=default;
    //移动构造函数、转移语义右值引用
    Any(Any&&) = default;
    Any& operator=(Any&&) = default;
    //禁止拷贝构造和赋值
    Any(const Any&)=delete;
    Any& operator=(const Any&)=delete;

    template<typename T>
    Any(T data):base_(std::make_unique<Derive<T>>(data)){}
    //将Any对象里面存储的数据data提取出来
    template<typename T>
    T case_(){
        //将基类Base指针转换为子类指针,使用get方法取出智能指针的裸指针
        Derive<T>* ptr = dynamic_cast<Derive<T>*>(base_.get());
        if(ptr==NULL){
            throw "type is unmatch！";
        }
        return ptr->data_;
    }
private:
    std::unique_ptr<Base> base_;
};


//信号量
class Semaphore{
public:
    Semaphore(size_t count=0):count_(count){}
    ~Semaphore()=default;
    //减少一个信号量
    void wait(){
        std::unique_lock<std::mutex> lock(mx_);
        cond_.wait(lock, [&](){return count_>0;});
        count_--;
    }
    //增加一个信号量
    void post(){
        std::unique_lock<std::mutex> lock(mx_);
        count_++;
        cond_.notify_all();
    }
private:
    size_t count_;
    std::mutex mx_; 
    std::condition_variable cond_;
};

//申明Task类型
class Task;

//实现接受提交到线程池的任务的返回类型
class Result{
public:
    Result(std::shared_ptr<Task> task, bool isVaild=true);
    ~Result();
    
    //获取task任务的返回值存储到Result对象中
    void setVal(Any any);

    //用户调用方法返回任务task的返回值
    Any get();
private:
    Any any_;
    Semaphore sem_;
    std::shared_ptr<Task> task_;
    std::atomic_bool isVaild_;
};

//枚举类型
enum PoolMode{
    MODE_FIXED,
    MODE_CACHED
};
class Task{
public:
    Task();
    ~Task();
    void execute();
    virtual Any run()=0;
    void setResult(Result* res);
private:
    //注意这个地方不能使用共享智能指针，会发生循环引用
    Result* result_;
};

class Thread{
public:
    using ThreadFunc = std::function<void(int)>;
    Thread(ThreadFunc func);
    ~Thread();
    //启动线程
    void start();
    //获取线程ID
    size_t getId();
private:
    //线程函数对象包装类型
    ThreadFunc func_;
    static size_t generateId_;  
    //线程ID
    size_t threadId_; 
};

class ThreadPool{
    public:
        ThreadPool();
        ~ThreadPool();
        ThreadPool(const ThreadPool&)=delete;
        ThreadPool& operator=(const ThreadPool&)=delete;
        //设置线程池的工作模式
        void setMode(PoolMode mode);
    
        //设置任务队列的上线阈值
        void setTaskQMaxThreshHold(size_t threshhold);    

        //提交任务
        Result submitTask(std::shared_ptr<Task> task);

        //设置线程池cached模式下，线程的个数的上线阈值
        void setThreadSzieThreshhold(int threshhold);

        //开启线程池
        void start(size_t initThreadSize = std::thread::hardware_concurrency());                                   
    private:
        //线程执行函数
        void threadFunc(int threadId);
        //检查线程池的运行状态
        bool checkRunningState() const;
    private:
        // std::vector<std::unique_ptr<Thread>> threads_;     //线程列表
        std::unordered_map<size_t, std::unique_ptr<Thread>> threads_;
        size_t initThreadSize_;                               //初始线程数量
        size_t threadSzieThreshhold_;                      //线程的个数上线阈值
        std::atomic_uint freeThreadSize_;                  //空闲线程的数量   
        std::atomic_uint curThreadSize_;                   //记录当前线程的总数       

        std::queue<std::shared_ptr<Task>> taskQueue_;     //任务队列
        std::atomic_uint taskSize_;                       //任务数量
        size_t taskQMaxThreshHold_;                       //任务队列上线阈值
        std::mutex taskQMutex_;                           //任务队列互斥锁
        std::condition_variable notFull_;                 //线程通信，表示任务队列不满
        std::condition_variable notEmpty_;                //线程通信，宝石任务队列不空
        std::condition_variable exitCond_;                //等待线程全部回收

        PoolMode poolMode_;                               //当前线程的模式
        std::atomic_bool isPoolRunning_;                   //线程池的运行状态
};
