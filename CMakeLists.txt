cmake_minimum_required(VERSION 3.16)
project(JonyServer)

set(CMAKE_CXX_STANDARD 11)
#SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L/usr/lib64/mysql -lmysqlclient ")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}  -pthread")

LINK_DIRECTORIES(/usr/lib/x86_64-linux-gnu)

add_executable(JonyServer main.cpp CGImysql/sql_connection_pool.h CGImysql/sql_connection_pool.cpp http/http_conn.h http/http_conn.cpp lock/locker.h log/block_queue.h log/log.cpp log/log.h threadpool/threadpool.h ConcurrentMemoryPool/CentralCache.h ConcurrentMemoryPool/CentralCache.cpp ConcurrentMemoryPool/Common.h ConcurrentMemoryPool/ConcurrentAlloc.h ConcurrentMemoryPool/PageCache.h ConcurrentMemoryPool/PageCache.cpp ConcurrentMemoryPool/ThreadCache.h ConcurrentMemoryPool/ThreadCache.cpp timer/time_wheel_timer.h timer/time_wheel_timer.cpp)
target_link_libraries(${PROJECT_NAME} libmysqlclient.so)
