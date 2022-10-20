/*
 * Copyright 2018-2020 Sergey Zolotarev
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <windows.h>

#define UNIT_KiB (1 << 10)
#define UNIT_MiB (1 << 20)
#define UNIT_GiB (1 << 30)
#define DEFAULT_BUFFER_SIZE 4096
#define UPDATE_INTERVAL 1000000
#define BUFFER_COUNT 2

#ifdef _MSC_VER
    #define strdup _strdup
    #define strtoll _strtoi64
    #define strtok_r strtok_s
#endif

struct program_options
{
    BOOL print_drive_list;
    const char *filename_in;
    const char *filename_out;
    size_t block_size;
    size_t count;
    const char *status;
};

struct program_state
{
    HANDLE in_file;
    HANDLE out_file;
    DWORD buffer_size;
    char *buffer[BUFFER_COUNT];
    BOOL out_file_is_device;
    BOOL started_copying;
    ULONGLONG start_time;
    size_t bytes_read;
    size_t bytes_write;
    size_t blocks_read;
    size_t blocks_write;
    size_t count;
    HANDLE semaphore_buffer_ready;
    HANDLE semaphore_buffer_occupied;
    HANDLE mutex_progress_display;
    HANDLE handle_thread_read;
    HANDLE handle_thread_write;
    DWORD bytes_read_per_block[BUFFER_COUNT];
    DWORD bytes_write_per_block;
};

static void print_usage(void)
{
    fprintf(stderr, "Usage: wdd if=<in_file> of=<out_file> [bs=N] [count=N] [status=progress]\n");
}

static ULONGLONG get_time_usec(void)
{
    FILETIME file_time;
    ULARGE_INTEGER time;

    GetSystemTimeAsFileTime(&file_time);
    time.LowPart = file_time.dwLowDateTime;
    time.HighPart = file_time.dwHighDateTime;
    return time.QuadPart / 10;
}

static void format_size(char *buffer, size_t buffer_size, size_t size)
{
    if (size >= UNIT_GiB)
    {
        snprintf(buffer, buffer_size, "%0.1f GiB", (double)size / (double)UNIT_GiB);
    }
    else if (size >= UNIT_MiB)
    {
        snprintf(buffer, buffer_size, "%0.1f MiB", (double)size / (double)UNIT_MiB);
    }
    else if (size >= UNIT_KiB)
    {
        snprintf(buffer, buffer_size, "%0.1f KiB", (double)size / (double)UNIT_KiB);
    }
    else
    {
        snprintf(buffer, buffer_size, "%zu B", size);
    }
}

static void format_speed(char *buffer, size_t buffer_size, double speed)
{
    if (speed >= (double)UNIT_GiB)
    {
        snprintf(buffer, buffer_size, "%0.1f GiB/s", speed / (double)UNIT_GiB);
    }
    else if (speed >= (double)UNIT_MiB)
    {
        snprintf(buffer, buffer_size, "%0.1f MiB/s", speed / (double)UNIT_MiB);
    }
    else if (speed >= (double)UNIT_KiB)
    {
        snprintf(buffer, buffer_size, "%0.1f KiB/s", speed / (double)UNIT_KiB);
    }
    else
    {
        snprintf(buffer, buffer_size, "%0.1f B/s", speed);
    }
}

static void print_progress(size_t bytes_copied,
                           size_t last_bytes_copied,
                           ULONGLONG start_time,
                           ULONGLONG last_time)
{
    ULONGLONG current_time;
    ULONGLONG elapsed_time;
    double speed;
    char bytes_str[16];
    char speed_str[16];

    current_time = get_time_usec();
    elapsed_time = current_time - start_time;
    if (elapsed_time >= UPDATE_INTERVAL)
    {
        speed = last_bytes_copied / ((double)(current_time - last_time) / UPDATE_INTERVAL);
    }
    else
    {
        speed = (double)last_bytes_copied;
    }

    format_size(bytes_str, sizeof(bytes_str), bytes_copied);
    format_speed(speed_str, sizeof(speed_str), speed);

    fprintf(stderr,
           "%zu B (%s) copied, %0.1f s, %s\n",
           bytes_copied,
           bytes_str,
           (double)elapsed_time / 1000000.0,
           speed_str);
}

static void print_status(size_t bytes_copied, ULONGLONG start_time)
{
    print_progress(
        bytes_copied,
        bytes_copied,
        start_time,
        start_time);
}

static void clear_output(void)
{
    HANDLE console;
    COORD start_coord = {0, 0};
    DWORD num_chars_written;
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;

    console = GetStdHandle(STD_ERROR_HANDLE);
    GetConsoleScreenBufferInfo(console, &buffer_info);
    start_coord.Y = buffer_info.dwCursorPosition.Y - 1;
    FillConsoleOutputCharacterA(
        console,
        ' ',
        buffer_info.dwSize.X,
        start_coord,
        &num_chars_written);
    SetConsoleCursorPosition(console, start_coord);
}

static char *get_error_message(DWORD error)
{
    char *buffer = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL,
        error,
        0,
        (char *)&buffer,
        0,
        NULL);
    return buffer;
}

static void cleanup(const struct program_state *state)
{
    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        VirtualFree(state->buffer[i], 0, MEM_RELEASE);
    }

    if (state->out_file_is_device)
    {
        DeviceIoControl(state->out_file, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, NULL, NULL);
    }

    if (state->in_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->in_file);
    }

    if (state->out_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->out_file);
    }

    if (state->handle_thread_read != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->handle_thread_read);
    }

    if (state->handle_thread_write != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->handle_thread_write);
    }

    if (state->semaphore_buffer_ready != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->semaphore_buffer_ready);
    }

    if (state->semaphore_buffer_occupied != INVALID_HANDLE_VALUE)
    {
        CloseHandle(state->semaphore_buffer_occupied);
    }
}

static void exit_on_error(const struct program_state *state,
                          int error_code,
                          char *format,
                          ...)
{
    va_list arg_list;
    char *reason;

    va_start(arg_list, format);
    vfprintf(stderr, format, arg_list);
    va_end(arg_list);
    fprintf(stderr, ": ");

    reason = get_error_message(error_code);
    reason[strlen(reason) - 2] = '\0';
    fprintf(stderr, "%s\n", reason);
    LocalFree(reason);

    if (state->started_copying)
    {
        print_status(state->bytes_write, state->start_time);
    }

    cleanup(state);
    exit(EXIT_FAILURE);
}

static size_t parse_size(const char *str)
{
    char *end = NULL;
    size_t size = (size_t)strtoll(str, &end, 10);

    if (end != NULL && *end != '\0')
    {
        switch (*end)
        {
            case 'k':
            case 'K':
                size *= UNIT_KiB;
                break;
            case 'm':
            case 'M':
                size *= UNIT_MiB;
                break;
            case 'g':
            case 'G':
                size *= UNIT_GiB;
                break;
        }
    }
    return size;
}

static BOOL is_empty_string(const char *str)
{
    return str == NULL || *str == '\0';
}

static BOOL parse_options(int argc,
                          char **argv,
                          struct program_options *options)
{
    options->filename_in = NULL;
    options->filename_out = NULL;
    options->block_size = 0;
    options->count = -1;
    options->status = NULL;

    for (int i = 1; i < argc; i++)
    {
        char *value = NULL;
        char *name = strtok_r(argv[i], "=", &value);

        if (strcmp(name, "list") == 0)
        {
            options->print_drive_list = TRUE;
            return TRUE;
        }
        else if (strcmp(name, "if") == 0)
        {
            options->filename_in = strdup(value);
        }
        else if (strcmp(name, "of") == 0)
        {
            options->filename_out = strdup(value);
        }
        else if (strcmp(name, "bs") == 0)
        {
            options->block_size = parse_size(value);
        }
        else if (strcmp(name, "count") == 0)
        {
            options->count = (size_t)strtoll(value, NULL, 10);
        }
        else if (strcmp(name, "status") == 0)
        {
            options->status = strdup(value);
        }
        else
        {
            return FALSE;
        }
    }

    return (is_empty_string(options->filename_in) || is_empty_string(options->filename_out)) == FALSE;
}

DWORD WINAPI thread_read(struct program_state *state)
{
    BOOL result;
    DWORD buffer_index;

    while (1)
    {
        if (state->count >= 0 && state->blocks_read >= state->count)
        {
            break;
        }
        buffer_index = state->blocks_read % BUFFER_COUNT;
        WaitForSingleObject(state->semaphore_buffer_ready, -1);
        result = ReadFile(
            state->in_file,
            state->buffer[buffer_index],
            state->buffer_size,
            &(state->bytes_read_per_block[buffer_index]),
            NULL);
        if (state->bytes_read_per_block[buffer_index] == 0 || (!result && GetLastError() == ERROR_SECTOR_NOT_FOUND))
        {
            ReleaseSemaphore(state->semaphore_buffer_occupied, 1, NULL);
            break;
        }
        if (result == FALSE)
        {
            ReleaseSemaphore(state->semaphore_buffer_occupied, 1, NULL);
            exit_on_error(state, GetLastError(), "Error reading from file");
        }

        state->bytes_read += state->bytes_read_per_block[buffer_index];
        state->blocks_read++;
        ReleaseSemaphore(state->semaphore_buffer_occupied, 1, NULL);
    }
    return 1;
}

DWORD WINAPI thread_write(struct program_state *state)
{
    BOOL result;
    DWORD buffer_index;

    while (1)
    {
        if (state->mutex_progress_display != INVALID_HANDLE_VALUE)
        {
            ReleaseSemaphore(state->mutex_progress_display, 1, NULL);
        }
        if (state->count >= 0 && state->blocks_write >= state->count)
        {
            break;
        }
        buffer_index = state->blocks_write % BUFFER_COUNT;
        WaitForSingleObject(state->semaphore_buffer_occupied, -1);
        if (state->bytes_read_per_block[buffer_index] == 0)
        {
            ReleaseSemaphore(state->semaphore_buffer_ready, 1, NULL);
            break;
        }
        result = WriteFile(
            state->out_file,
            state->buffer[buffer_index],
            state->bytes_read_per_block[buffer_index],
            &(state->bytes_write_per_block),
            NULL);
        if (result == FALSE)
        {
            ReleaseSemaphore(state->semaphore_buffer_ready, 1, NULL);
            exit_on_error(state, GetLastError(), "Error writing to file");
        }

        state->bytes_write += state->bytes_write_per_block;
        state->blocks_write++;
        ReleaseSemaphore(state->semaphore_buffer_ready, 1, NULL);
    }
    state->started_copying = FALSE;
    return 1;
}

int main(int argc, char **argv)
{
    struct program_options options;
    struct program_state state;
    size_t num_blocks_copied = 0;
    BOOL show_progress = FALSE;
    size_t last_bytes_copied = 0;
    ULONGLONG last_time = 0;
    DISK_GEOMETRY_EX disk_geometry;

    ZeroMemory(&options, sizeof(options));

    if (parse_options(argc, argv, &options) == FALSE)
    {
        print_usage();
        return EXIT_FAILURE;
    }

    if (options.print_drive_list)
    {
        return system("wmic diskdrive list brief");
    }

    ZeroMemory(&state, sizeof(state));
    state.in_file = INVALID_HANDLE_VALUE;
    state.out_file = INVALID_HANDLE_VALUE;
    state.mutex_progress_display = INVALID_HANDLE_VALUE;
    state.start_time = get_time_usec();
    state.out_file_is_device = FALSE;
    state.started_copying = FALSE;
    state.bytes_read = 0;
    state.bytes_write = 0;
    state.blocks_read = 0;
    state.blocks_write = 0;

    if (strcmp(options.filename_in, "-") == 0)
    {
        state.in_file = GetStdHandle(STD_INPUT_HANDLE);
        if (state.in_file == INVALID_HANDLE_VALUE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Could not open stdin for reading");
        }
    }
    else
    {

        state.in_file = CreateFileA(
            options.filename_in,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            NULL);
        if (state.in_file == INVALID_HANDLE_VALUE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Could not open input file or device %s for reading",
                options.filename_in);
        }
    }

    if (strcmp(options.filename_out, "-") == 0)
    {
        state.out_file = GetStdHandle(STD_OUTPUT_HANDLE);
        if (state.out_file == INVALID_HANDLE_VALUE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Could not open stdout for writing");
        }
    }
    else
    {
        /* First try to open as an existing file, thne as a new file. We can't
         * use OPEN_ALWAYS because it fails when out_file is a physical drive
         * (no idea why).
         */
        state.out_file = CreateFileA(
            options.filename_out,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (state.out_file == INVALID_HANDLE_VALUE)
        {
            state.out_file = CreateFileA(
                options.filename_out,
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL);
        }
        if (state.out_file == INVALID_HANDLE_VALUE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Could not open output file or device %s for writing",
                options.filename_out);
        }
    }

    state.buffer_size = DEFAULT_BUFFER_SIZE;
    state.out_file_is_device = DeviceIoControl(
        state.out_file,
        IOCTL_DISK_GET_DRIVE_GEOMETRY,
        NULL,
        0,
        &disk_geometry,
        sizeof(disk_geometry),
        NULL,
        NULL);

    if (state.out_file_is_device)
    {
        DWORD sector_size;

        if (DeviceIoControl(state.out_file, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, NULL, NULL) == FALSE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Failed to dismount output volume");
        }
        if (DeviceIoControl(state.out_file, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, NULL, NULL) == FALSE)
        {
            exit_on_error(
                &state,
                GetLastError(),
                "Failed to lock output volume");
        }

        sector_size = disk_geometry.Geometry.BytesPerSector;
        if (options.block_size < sector_size)
        {
            state.buffer_size = sector_size;
        }
        else
        {
            state.buffer_size = (state.buffer_size / sector_size) * sector_size;
        }
    }
    else
    {
        if (options.block_size > 0)
        {
            state.buffer_size = (DWORD)options.block_size; // TODO: Possible bug with bs > 4GB
        }
    }

    for (int i = 0; i < BUFFER_COUNT; i++)
    {
        state.buffer[i] = VirtualAlloc(
            NULL,
            state.buffer_size,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE);
        if (state.buffer[i] == NULL)
        {
            exit_on_error(&state, GetLastError(), "Failed to allocate buffer");
        }
    }
    state.count = options.count;

    fprintf(stderr, "\n");
    show_progress = (options.status != NULL && strcmp(options.status, "progress") == 0);
    state.started_copying = TRUE;

    state.semaphore_buffer_ready = CreateSemaphore(NULL, BUFFER_COUNT, BUFFER_COUNT, NULL);
    state.semaphore_buffer_occupied = CreateSemaphore(NULL, 0, BUFFER_COUNT, NULL);
    if (show_progress == TRUE)
    {
        state.mutex_progress_display = CreateSemaphore(NULL, 0, 1, NULL);
    }
    state.handle_thread_read = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)thread_read, &state, 0, NULL);
    state.handle_thread_write = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)thread_write, &state, 0, NULL);

    if (show_progress == TRUE)
    {
        while (state.started_copying == TRUE)
        {
            ULONGLONG current_time;
            WaitForSingleObject(state.mutex_progress_display, -1);
            current_time = get_time_usec();
            if (last_time == 0)
            {
                last_time = current_time;
            }
            else
            {
                if (current_time - last_time >= UPDATE_INTERVAL)
                {
                    clear_output();
                    print_progress(
                        state.bytes_write,
                        state.bytes_write - last_bytes_copied,
                        state.start_time,
                        last_time);
                    last_time = current_time;
                    last_bytes_copied = state.bytes_write;
                }
            }
        }
        CloseHandle(state.mutex_progress_display);
    }

    WaitForSingleObject(state.handle_thread_read, -1);
    WaitForSingleObject(state.handle_thread_write, -1);

    cleanup(&state);
    clear_output();
    print_status(state.bytes_write, state.start_time);

    return EXIT_SUCCESS;
}
