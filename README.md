# 基于c++11fixed&&cached模式线程池

## 技术要点:

1.使用c++语言级别的thread创建线程，配合绑定器和function函数对象传线程函数

2.使用智能指针代替裸指针

3.使用c++语言级别的互斥锁，条件变量进行线程同步

4.使用c++11packaged_task包装异步任务并获取返回值

5.使用可变参函数模板

6.使用decltype类型推导

## example

### 1.并行计算

```c++
#include "threadpool.h"
#include <iostream>

int main(){
    ThreadPool threadPool;
    //使用默认参数 工作在fixed模式下
    threadPool.start();

    std::function<int(int,int)> func=[](int a,int b)->int{
        int sum=0;
        for (int i = a; i < b; ++i) {
            sum+=i;
        }
        return sum;
    };
    std::future<int> res1=threadPool.submit(func,1,100);
    std::future<int> res2=threadPool.submit(func,100,200);
    std::cout<<"Parallel results: "<<res1.get()+res2.get()<<std::endl;
    int sum=0;
    for(int i = 1; i < 200; ++i) {
        sum+=i;
    }
    std::cout<<"serial results: "<<sum<<std::endl;

}
```

输出结果：

```
Parallel results: 19900
serial results: 19900
```

