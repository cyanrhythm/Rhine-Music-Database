#include "ProcessMetrics.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#elif defined(Q_OS_LINUX)
#include <QFile>
#include <unistd.h>
#endif
#ifndef Q_OS_WIN
#include <sys/resource.h>
#endif

/** 读取主进程的当前常驻内存与累计 CPU 时间；读取失败的指标保留为不可用。 */
ProcessMetrics readProcessMetrics() {
    ProcessMetrics result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) {
        result.residentBytes = qint64(memory.WorkingSetSize);
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        const auto nanoseconds = [](FILETIME value) {
            return qint64((quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime) * 100;
        };
        result.cpuNanoseconds = nanoseconds(kernel) + nanoseconds(user);
    }
#else
#ifdef Q_OS_MACOS
    mach_task_basic_info memory{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&memory), &count) ==
        KERN_SUCCESS) {
        result.residentBytes = qint64(memory.resident_size);
    }
#elif defined(Q_OS_LINUX)
    // statm 的第二列是当前常驻页数，乘以实际页大小；不使用历史峰值代替当前内存。
    QFile memory(QStringLiteral("/proc/self/statm"));
    if (memory.open(QIODevice::ReadOnly)) {
        const auto fields = memory.readAll().simplified().split(' ');
        bool validResidentPages = false;
        const auto residentPages = fields.value(1).toLongLong(&validResidentPages);
        const auto pageBytes = sysconf(_SC_PAGESIZE);
        if (validResidentPages && residentPages >= 0 && pageBytes > 0) {
            result.residentBytes = residentPages * qint64(pageBytes);
        }
    }
#endif
    // macOS 与 Linux 都通过 getrusage 累计用户态和内核态 CPU 时间，统一转换为纳秒。
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        result.cpuNanoseconds = (qint64(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1000000000 +
                                (qint64(usage.ru_utime.tv_usec) + usage.ru_stime.tv_usec) * 1000;
    }
#endif
    return result;
}
