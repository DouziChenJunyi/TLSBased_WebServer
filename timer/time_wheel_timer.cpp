#include <time.h>
#include "../http/http_conn.h"
#include "../log/log.h"
#include "time_wheel_timer.h"


time_wheel::~time_wheel() {
    for (int i = 0; i < N; i++) {
        time_wheel_timer *tmp = slots[i];
        while (tmp) {
            slots[i] = tmp->next;
            delete tmp;
            tmp = slots[i];
        }
    }
}

time_wheel_timer *time_wheel::add_timer(int timeout) {
    if (timeout < 0) {
        return NULL;
    }
    int ticks = 0;
    if (timeout < SI) {
        ticks = 1;
    }
    else {
        ticks = timeout / SI;
    }
    // 计算待插入的定时器在时间轮转动多少圈后被触发
    int rotation = ticks / N;
    // 计算待插入的定时器应该被插入哪个槽中
    int ts = (cur_slot + (ticks % N)) % N;
    // 创建新的定时器，它在时间轮转动rotation圈之后被触发，且位于第ts个槽上
    time_wheel_timer *timer = new time_wheel_timer(rotation, ts);
    // 如果第ts个槽中尚无任何定时器，则把新建的定时器插入其中，并将该定时器设置为该槽的头结点
    if (!slots[ts]) {
        slots[ts] = timer;
    }
    else {
        timer->next = slots[ts];
        slots[ts]->prev = timer;
        slots[ts] = timer;
    }
    return timer;
}

void time_wheel::del_timer(time_wheel_timer *timer) {
    if (!timer) {
        return;
    }
    int ts = timer->time_slot;
    // slots[ts] 是目标定时器所在槽的头结点，如果目标定时器就是头结点，则需要重置第ts个槽的头结点
    if (timer == slots[ts]) {
        slots[ts] = slots[ts]->next;
        if (slots[ts]) {
            slots[ts]->prev = NULL;
        }
        delete timer;
    }
    else {
        timer->prev->next = timer->next;
        if (timer->next) {
            timer->next->prev = timer->prev;
        }
        delete timer;
    }
}

