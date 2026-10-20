#pragma once
#include <QtGlobal>

struct ProcessMetrics {
    qint64 residentBytes = -1;
    qint64 cpuNanoseconds = -1;
};

// 仅统计主程序；Node 音乐桥接子进程不计入这里的内存和 CPU。
ProcessMetrics readProcessMetrics();
