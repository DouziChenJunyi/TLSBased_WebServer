#ifndef TIME_WHEEL_TIMER
#define TIME_WHEEL_TIMER

#include <time.h>
#include "../http/http_conn.h"
#include "../log/log.h"

#define BUFFER_SIZE 64

class time_wheel_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    char buf[BUFFER_SIZE];
    time_wheel_timer *timer;
};

class time_wheel_timer
{
public:
    int rotation; // 记录定时器在时间轮转多少圈后失效
    int time_slot; // 记录定时器属于时间轮上的哪个槽
    void (*cb_func)(client_data*); // 定时器回调函数
    client_data *user_data; //客户数据
    time_wheel_timer *next;
    time_wheel_timer *prev;

public:
    time_wheel_timer(int rot, int ts) : next(NULL), prev(NULL), rotation(rot), time_slot(ts) {}
};

class time_wheel
{
private:
    /* data */
    static const int N = 60; // 时间轮上的槽的数目
    static const int SI = 1; // 每1s时间轮轮动一次，槽间隔为1s
    time_wheel_timer *slots[N]; // 时间轮的槽，其中每个元素指向一个定时器链表，链表无序
    int cur_slot; // 时间轮的当前槽

public:
    time_wheel() : cur_slot(0) {
        for (int i = 0; i < N; i++) {
            slots[i] = NULL;
        }
    }
    ~time_wheel();
    // 根据定时值 timeout 创建一个定时器，并把它插入合适槽中
    time_wheel_timer *add_timer(int timeout);

    // 删除定时器
    void del_timer(time_wheel_timer *timer);

    // SI时间到后，调用该函数，时间轮向前滚动一个槽的间隔
    void tick();
};

#endif