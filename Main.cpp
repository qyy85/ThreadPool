#include<iostream>
#include<chrono>
#include<thread>
#include"head/threadpool.h"
using namespace std;

int sum(int a, int b){
    return a+b;
}
int sum1(int a, int b, int c){
    this_thread::sleep_for(chrono::seconds(3));
    return a+b+c;
}
int main(){
    
    ThreadPool pool;
    pool.setMode(PoolMode::MODE_CACHED);
    pool.start();
    for (size_t i = 0; i < 1024; i++)
    {
        pool.submitTask(sum1, i, i+1, i+2);
    }
    
    this_thread::sleep_for(chrono::seconds(10));
    return 0;
}