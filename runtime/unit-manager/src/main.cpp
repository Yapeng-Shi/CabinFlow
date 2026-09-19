/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <iostream>

#include "all.h"

#include "zmq_bus.h"
#include "remote_action.h"
#include "remote_server.h"
#include "unit_data.h"

pthread_spinlock_t key_sql_lock;

// KV 数据库，存储全局的元数据信息：系统全局配置、TASK、 channel url 等
std::unordered_map<std::string, std::any> key_sql;

// ZMQ 地址模板字符串
std::string zmq_s_format;
std::string zmq_c_format;

// 主线程退出标志，控制主循环的运行，通过信号处理函数设置为 1 来触发退出流程
int main_exit_flage = 0;

void get_run_config()
{
    load_default_config();
}

void tcp_work();

void tcp_stop_work();

void all_work()
{
    // 从全局配置 key_sql 取出 ZMQ 地址模板 zmq_s_format 和 zmq_c_format
    zmq_s_format = std::any_cast<std::string>(key_sql["config_zmq_s_format"]);
    zmq_c_format = std::any_cast<std::string>(key_sql["config_zmq_c_format"]);

    // 启动 remote_server_work：远程服务器工作线程，负责系统 RPC/管理能力
    remote_server_work();

    // 启动 tcp_work：TCP 接入和消息处理
    tcp_work();
}

void all_stop_work()
{
    tcp_stop_work();
    remote_server_stop_work();
}

static void __sigint(int iSigNo)
{
    printf("llm_sys will be exit!\n");

    // 设置退出标记 main_exit_flage = 1，通知主循环结束
    main_exit_flage = 1;
    ALOGD("llm_sys stop");
    all_stop_work();
    pthread_spin_destroy(&key_sql_lock);
}

void all_work_check()
{
}

int main(int argc, char *argv[])
{
    // 注册信号处理函数（SIGTERM、SIGINT），用于接管 Ctrl+C 或 kill 的退出流程
    signal(SIGTERM, __sigint);
    signal(SIGINT, __sigint);

    // 创建运行目录
    mkdir("/tmp/llm", 0777);

    // 初始化全局自旋锁 key_sql_lock（保护全局配置表并发访问）
    if (pthread_spin_init(&key_sql_lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        ALOGE("key_sql_lock init false");
        exit(1);
    }
    ALOGD("llm_sys start");
    // 加载默认配置
    get_run_config();

    all_work();
    ALOGD("llm_sys work");

    //  while 循环让主线程常驻
    while (main_exit_flage == 0) {
        sleep(1);
    }

    return 0;
}