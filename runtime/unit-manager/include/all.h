/*
 * SPDX-FileCopyrightText: 2024 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <pthread.h>
#include "sample_log.h"

#include <map>
#include <string>
#include <memory>
#include <stdexcept>
#include <utility>

#include <any>
#include <unordered_map>

extern std::unordered_map<std::string, std::any>
    key_sql;                             // 全局 KV 数据库，用于存储系统配置和状态信息，std::any 支持任意类型的值
extern pthread_spinlock_t key_sql_lock;  // 保护 key_sql 的自旋锁（适用于高频访问场景）

// do-while(0) 结构：避免宏展开后的语法错误
// 线程安全的 KV 操作宏：使用自旋锁保护对全局 key_sql 的访问，提供读取、设置和删除功能

// key_sql 读取指定 key 的值
#define SAFE_READING(_val, _type, _key)                    \
    do {                                                   \
        pthread_spin_lock(&key_sql_lock);                  \
        try {                                              \
            _val = std::any_cast<_type>(key_sql.at(_key)); \
        } catch (...) {                                    \
        }                                                  \
        pthread_spin_unlock(&key_sql_lock);                \
    } while (0)

// key_sql 设置指定 key 的值
#define SAFE_SETTING(_key, _val)            \
    do {                                    \
        pthread_spin_lock(&key_sql_lock);   \
        try {                               \
            key_sql[_key] = _val;           \
        } catch (...) {                     \
        }                                   \
        pthread_spin_unlock(&key_sql_lock); \
    } while (0)

//  key_sql 删除指定 key 的条目
#define SAFE_ERASE(_key)                    \
    do {                                    \
        pthread_spin_lock(&key_sql_lock);   \
        try {                               \
            key_sql.erase(_key);            \
        } catch (...) {                     \
        }                                   \
        pthread_spin_unlock(&key_sql_lock); \
    } while (0)

void load_default_config();
void unit_action_match(int com_id, const std::string &json_str);

extern std::string zmq_s_format;
extern std::string zmq_c_format;
extern int main_exit_flage;
