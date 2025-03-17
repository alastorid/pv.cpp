#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <process.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")


#define INITIAL_BUFFER_SIZE (1ULL << 24)
#define BLOCK_SIZE (1ULL << 20)
#define MIN_BUFFER_SIZE (1ULL << 22)
#define UPDATE_INTERVAL_MS 1000

#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_RESET   "\x1b[0m"
#define ANSI_CLEAR   "\x1b[2K"

typedef struct {
    LPVOID buffer;
    volatile SIZE_T committed_size;
    volatile LONG64 read_pos;
    volatile LONG64 write_pos;
    SIZE_T max_size;
    volatile BOOL should_stop;
    volatile ULONGLONG total_bytes_read;
    volatile ULONGLONG total_bytes_written;
    ULONGLONG start_time;
    HANDLE stdin_handle;
    HANDLE stdout_handle;
    CRITICAL_SECTION resize_lock;
    HANDLE writer_event;
    SIZE_T large_page_size;
} RingBuffer;

void GetSystemMemoryInfo(ULONGLONG* total_bytes, ULONGLONG* available_bytes);

void FormatBytes(ULONGLONG bytes, LPSTR buffer) {
    StrFormatByteSize64A(bytes, buffer, 32);
}
void FormatSpeed(ULONGLONG bytes, double seconds, LPSTR buffer) {
    double speed = bytes / seconds;
    CHAR size[32];
    StrFormatByteSize64A(speed, size, 32);
    sprintf(buffer, "%s/s", size);
}

BOOL EnableLargePages() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return FALSE;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }

    BOOL result = AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, 0);
    BOOL error = GetLastError();
    CloseHandle(token);

    return result && error == ERROR_SUCCESS;
}

SIZE_T GetLargePageSize() {
    SIZE_T size = GetLargePageMinimum();
    return size ? size : (2 * 1024 * 1024); // Default to 2MB if large pages not available
}

BOOL ResizeBuffer(RingBuffer* rb, SIZE_T new_size) {
    EnterCriticalSection(&rb->resize_lock);

    if (new_size == rb->committed_size) {
        LeaveCriticalSection(&rb->resize_lock);
        return TRUE;
    }

    if (new_size < MIN_BUFFER_SIZE) new_size = MIN_BUFFER_SIZE;
    if (new_size > rb->max_size) new_size = rb->max_size;

    // Round up to large page size
    new_size = (new_size + rb->large_page_size - 1) & ~(rb->large_page_size - 1);

    if (new_size > rb->committed_size) {
        LPVOID new_mem = VirtualAlloc((BYTE*)rb->buffer + rb->committed_size,
                                     new_size - rb->committed_size,
                                     MEM_COMMIT | MEM_LARGE_PAGES,
                                     PAGE_READWRITE);
        if (!new_mem) {
            // Fallback to regular pages if large pages fail
            new_mem = VirtualAlloc((BYTE*)rb->buffer + rb->committed_size,
                                 new_size - rb->committed_size,
                                 MEM_COMMIT,
                                 PAGE_READWRITE);
            if (!new_mem) {
                LeaveCriticalSection(&rb->resize_lock);
                return FALSE;
            }
        }
    } else if (new_size < rb->committed_size) {
        VirtualFree((BYTE*)rb->buffer + new_size,
                   rb->committed_size - new_size,
                   MEM_DECOMMIT);
    }

    rb->committed_size = new_size;
    LeaveCriticalSection(&rb->resize_lock);
    return TRUE;
}

unsigned __stdcall reader_thread(void* arg) {
    RingBuffer* rb = (RingBuffer*)arg;
    BYTE* read_buf = (BYTE*)rb->buffer;
    DWORD bytes_read;

    while (!rb->should_stop) {
        LONG64 current_write = rb->write_pos;
        LONG64 current_read = rb->read_pos;
        LONG64 used_space = current_write - current_read;

        if (used_space == 0 && current_write > 0) {
            if (WaitForSingleObject(rb->writer_event, 0) == WAIT_TIMEOUT) {
                EnterCriticalSection(&rb->resize_lock);
                rb->read_pos = rb->write_pos = 0;
                if (rb->committed_size > INITIAL_BUFFER_SIZE) {
                    ResizeBuffer(rb, INITIAL_BUFFER_SIZE);
                }
                LeaveCriticalSection(&rb->resize_lock);
                continue;
            }
        }

        if (used_space + BLOCK_SIZE > rb->committed_size) {
            SIZE_T needed_size = rb->committed_size * 2;
            while((used_space + BLOCK_SIZE) > needed_size)
            {
                needed_size *= 2;
            }

            if (!ResizeBuffer(rb, needed_size)) {
                // it's not a fatal error
            }
        }

        if (used_space + BLOCK_SIZE > rb->committed_size) {
            Sleep(30);
            continue;
        }

        SIZE_T write_offset = current_write % rb->committed_size;
        SIZE_T chunk_size = min(BLOCK_SIZE, rb->committed_size - write_offset);

        if (ReadFile(rb->stdin_handle, read_buf + write_offset, (DWORD)chunk_size, &bytes_read, NULL)) {
            if (bytes_read == 0) {
                rb->should_stop = TRUE;
                break;
            }
            InterlockedExchangeAdd64(&rb->write_pos, bytes_read);
            InterlockedExchangeAdd64((LONG64*)&rb->total_bytes_read, bytes_read);
            SetEvent(rb->writer_event);
        } else {
            rb->should_stop = TRUE;
            break;
        }
    }
    SetEvent(rb->writer_event);
    return 0;
}

unsigned __stdcall writer_thread(void* arg) {
    RingBuffer* rb = (RingBuffer*)arg;
    BYTE* write_buf = (BYTE*)rb->buffer;

    while (!rb->should_stop || rb->read_pos < rb->write_pos) {
        if (WaitForSingleObject(rb->writer_event, INFINITE) == WAIT_OBJECT_0) {
            while (rb->read_pos < rb->write_pos) {
                LONG64 current_read = rb->read_pos;
                SIZE_T read_offset = current_read % rb->committed_size;
                SIZE_T available = rb->write_pos - current_read;
                SIZE_T chunk_size = min(BLOCK_SIZE, min(available, rb->committed_size - read_offset));
                DWORD bytes_written;

                if (WriteFile(rb->stdout_handle, write_buf + read_offset, (DWORD)chunk_size, &bytes_written, NULL)) {
                    InterlockedExchangeAdd64(&rb->read_pos, bytes_written);
                    InterlockedExchangeAdd64((LONG64*)&rb->total_bytes_written, bytes_written);
                } else {
                    rb->should_stop = TRUE;
                    break;
                }
            }

            if (rb->read_pos == rb->write_pos && !rb->should_stop) {
                ResetEvent(rb->writer_event);
            }
        }
    }
    return 0;
}


unsigned __stdcall status_thread(void* arg) {
    RingBuffer* rb = (RingBuffer*)arg;
    const int MAX_STATUS_LEN = 4096;
    char status_buffer[MAX_STATUS_LEN];
    char read_size[32], write_size[32], buffer_size[32], commit_size[32], reserve_size[32];
    char read_speed[64], write_speed[64];
    char sys_total[32], sys_avail[32];
    HANDLE hConsole = GetStdHandle(STD_ERROR_HANDLE);
    DWORD written;

    // Variables for low-pass filter
    static double filtered_read_speed = 0.0;
    static double filtered_write_speed = 0.0;
    static ULONGLONG last_bytes_read = 0;
    static ULONGLONG last_bytes_written = 0;
    static ULONGLONG last_time = 0;
    const double alpha = 0.5; // Filter coefficient

    while (!rb->should_stop || rb->read_pos < rb->write_pos) {
        ULONGLONG current_time = GetTickCount64();
        double elapsed_sec = (current_time - rb->start_time) / 1000.0;
        double buffer_usage = (double)(rb->write_pos - rb->read_pos) / rb->committed_size * 100.0;
        double commit_usage = (double)rb->committed_size / rb->max_size * 100.0;

        // Calculate instantaneous speeds
        if (last_time > 0) {
            double dt = (current_time - last_time) / 1000.0;
            if (dt > 0) {
                double instant_read_speed = (rb->total_bytes_read - last_bytes_read) / dt;
                double instant_write_speed = (rb->total_bytes_written - last_bytes_written) / dt;

                // Apply low-pass filter
                filtered_read_speed = alpha * filtered_read_speed + (1.0 - alpha) * instant_read_speed;
                filtered_write_speed = alpha * filtered_write_speed + (1.0 - alpha) * instant_write_speed;
            }
        }

        // Update last values
        last_time = current_time;
        last_bytes_read = rb->total_bytes_read;
        last_bytes_written = rb->total_bytes_written;

        ULONGLONG total_mem, avail_mem;
        GetSystemMemoryInfo(&total_mem, &avail_mem);

        FormatBytes(rb->total_bytes_read, read_size);
        FormatBytes(rb->total_bytes_written, write_size);
        FormatBytes(rb->write_pos - rb->read_pos, buffer_size);
        FormatBytes(rb->committed_size, commit_size);
        FormatBytes(rb->max_size, reserve_size);

        // Format filtered speeds using existing FormatSpeed function
        char filtered_read_speed_str[64], filtered_write_speed_str[64];
        FormatSpeed((ULONGLONG)filtered_read_speed, 1.0, filtered_read_speed_str);
        FormatSpeed((ULONGLONG)filtered_write_speed, 1.0, filtered_write_speed_str);

        int len = snprintf(status_buffer, MAX_STATUS_LEN,
            "\033[1;36mRead:\033[0m %s \033[32m(%s)\033[0m | "
            "\033[1;36mWritten:\033[0m %s \033[32m(%s)\033[0m | "
            "\033[1;36mBuffer:\033[0m %s | "
            "\033[1;36mMem:\033[0m %s/%s \033[33m(%.1f%% committed)\033[0m\n",
            read_size, filtered_read_speed_str,
            write_size, filtered_write_speed_str,
            buffer_size,
            commit_size, reserve_size, commit_usage);

        fprintf(stderr, "%s", status_buffer);
        fflush(stderr);

        snprintf(status_buffer, MAX_STATUS_LEN,
            "PV - %s @ %s | Buf: %.1f%% | Mem: %.1f%%",
            read_size, filtered_read_speed_str, buffer_usage, commit_usage);
        SetConsoleTitleA(status_buffer);

        Sleep(UPDATE_INTERVAL_MS);
    }

    return 0;
}

void GetSystemMemoryInfo(ULONGLONG* total_bytes, ULONGLONG* available_bytes) {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    *total_bytes = memInfo.ullTotalPhys;
    *available_bytes = memInfo.ullAvailPhys;
}

int main(void) {
    HANDLE hOut = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const ULONGLONG gb = 1024ull * 1024ull * 1024ull;
    ULONGLONG total_bytes;
    ULONGLONG available_bytes;

    GetSystemMemoryInfo(&total_bytes, &available_bytes);

    RingBuffer rb = {0};
    rb.stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    rb.stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    rb.max_size = available_bytes/2;
    rb.start_time = GetTickCount64();
    InitializeCriticalSection(&rb.resize_lock);
    rb.writer_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    // Enable large pages
    EnableLargePages();
    rb.large_page_size = GetLargePageSize();

    // Round up sizes to large page boundaries
    rb.max_size = (rb.max_size + rb.large_page_size - 1) & ~(rb.large_page_size - 1);
    SIZE_T initial_size = (INITIAL_BUFFER_SIZE + rb.large_page_size - 1) & ~(rb.large_page_size - 1);

    // Reserve the full address space
    rb.buffer = VirtualAlloc(NULL, rb.max_size, MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (!rb.buffer) {
        // Fallback to regular pages if large pages fail
        rb.buffer = VirtualAlloc(NULL, rb.max_size, MEM_RESERVE, PAGE_READWRITE);
        if (!rb.buffer) {
            fprintf(stderr, "Failed to reserve memory\n");
            return 1;
        }
    }

    // Commit initial buffer size with large pages
    LPVOID commit_result = VirtualAlloc(rb.buffer, initial_size,
                                      MEM_COMMIT | MEM_LARGE_PAGES,
                                      PAGE_READWRITE);
    if (!commit_result) {
        // Fallback to regular pages
        commit_result = VirtualAlloc(rb.buffer, initial_size,
                                   MEM_COMMIT,
                                   PAGE_READWRITE);
        if (!commit_result) {
            VirtualFree(rb.buffer, 0, MEM_RELEASE);
            fprintf(stderr, "Failed to commit initial memory\n");
            return 1;
        }
    }
    rb.committed_size = initial_size;

    HANDLE threads[3];
    threads[0] = (HANDLE)_beginthreadex(NULL, 0, reader_thread, &rb, 0, NULL);
    threads[1] = (HANDLE)_beginthreadex(NULL, 0, writer_thread, &rb, 0, NULL);
    threads[2] = (HANDLE)_beginthreadex(NULL, 0, status_thread, &rb, 0, NULL);

    SetThreadPriority(threads[0], THREAD_PRIORITY_HIGHEST);
    SetThreadPriority(threads[1], THREAD_PRIORITY_ABOVE_NORMAL);

    WaitForMultipleObjects(3, threads, TRUE, INFINITE);

    DeleteCriticalSection(&rb.resize_lock);
    CloseHandle(rb.writer_event);
    VirtualFree(rb.buffer, 0, MEM_RELEASE);

    for (int i = 0; i < 3; i++) {
        CloseHandle(threads[i]);
    }

    return 0;
}
