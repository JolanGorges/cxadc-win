// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * levelmon - Windows port of the cxadc levelmon tool
 *
 * Continuously reads samples from a cxadc-win device and prints
 * clipping, amplitude and DC-offset statistics, plus the driver's
 * DMA over/underflow counter and the measured sample rate.
 *
 * Based on levelmon.c from cxadc-linux3 (Ethan Halsall, 2024).
 * Windows port 2026, same license.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <cx_ctl_codes.h>
#include <cx_config.h>

static volatile LONG g_stop = 0;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

static LPCSTR win32_err(DWORD id)
{
    static char msg[512];
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
        NULL, id, 0, msg, sizeof(msg), NULL);
    if (n > 0 && msg[n - 1] == ' ')
        msg[n - 1] = '\0';
    return n ? msg : "unknown error";
}

static void usage(const char* prog)
{
    fprintf(stderr,
        "levelmon continuously monitors levels from a cxadc-win card.\n"
        "\n"
        "Usage: %s [device] [-i seconds] [-n count] [-b mib] [-h]\n"
        "\n"
        "  device      \\\\.\\cxadc0 (default), or just the index, e.g. 1\n"
        "  -i seconds  data read per printed line, in seconds at 40 MS/s (default 0.25)\n"
        "  -n count    stop after this many lines (default: run until Ctrl+C)\n"
        "  -b mib      bytes read per line in MiB, overrides -i\n"
        "\n"
        "Output format:\n"
        ".   / clipped samples low\n"
        ".   |     / min amplitude\n"
        ".   |     |         / avg amplitude (negative side of center)\n"
        ".   |     |         |                / dc offset\n"
        ".   |     |         |                |\n"
        "lo |0| [  3.906%%] ( 33.429%%) center -0.54%% hi ( 65.764%%) [ 95.312%%] |0|  nsamp 10000000  rate 28.64  ouflow 0\n"
        ".   ^     ^^^^^     ^^^^^^           ^^^^       ^^^^^^     ^^^^^^    ^        ^^^^^^^^       ^^^^^         ^\n"
        ".                                               |          |         |        |              |             \\ driver DMA over/underflow count\n"
        ".                                               |          |         |        |              \\ measured sample rate in MS/s\n"
        ".                                               |          |         |        \\ samples in this line\n"
        ".                                               |          |         \\ clipped samples high\n"
        ".                                               |          \\ max amplitude\n"
        ".                                               \\ avg amplitude (positive side of center)\n",
        prog);
}

int main(int argc, char* argv[])
{
    char device_path[64] = "\\\\.\\cxadc0";
    double interval = 0.25;
    long max_lines = 0;
    size_t chunk_bytes = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "/?")) {
            usage(argv[0]);
            return EXIT_FAILURE;
        } else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            interval = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            max_lines = atol(argv[++i]);
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            chunk_bytes = (size_t)atol(argv[++i]) * 1024 * 1024;
        } else if (argv[i][0] != '-') {
            if (strlen(argv[i]) <= 2 && argv[i][0] >= '0' && argv[i][0] <= '9')
                snprintf(device_path, sizeof(device_path), "\\\\.\\cxadc%s", argv[i]);
            else
                snprintf(device_path, sizeof(device_path), "%s", argv[i]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (interval <= 0.01)
        interval = 0.25;
    if (chunk_bytes == 0)
        chunk_bytes = (size_t)(40.0e6 * interval); // sized for a 40 MS/s card, slower cards just take longer per line
    chunk_bytes &= ~(size_t)0xFFF;               // whole pages
    if (chunk_bytes < 0x100000)
        chunk_bytes = 0x100000;
    if (chunk_bytes > 32u * 1024 * 1024)
        chunk_bytes = 32u * 1024 * 1024;         // stay well under the 64 MiB DMA ring

    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    HANDLE dev = CreateFileA(device_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open %s: %s\n", device_path, win32_err(GetLastError()));
        return EXIT_FAILURE;
    }

    DEVICE_CONFIG cfg;
    DWORD ret_len = 0;
    if (!DeviceIoControl(dev, CX_IOCTL_CONFIG_GET, NULL, 0, &cfg, sizeof(cfg), &ret_len, NULL)) {
        fprintf(stderr, "CX_IOCTL_CONFIG_GET failed: %s\n", win32_err(GetLastError()));
        CloseHandle(dev);
        return EXIT_FAILURE;
    }

    const int tenbit = cfg.tenbit ? 1 : 0;
    const unsigned maxv = tenbit ? 0x400 : 0x100;   // exclusive top, as in the Linux tool
    const unsigned minv = 1;
    const unsigned center = maxv / 2;

    printf("%s  vmux=%lu level=%lu sixdb=%s tenbit=%s center_offset=%lu  (%zu MiB per line)\n",
        device_path, cfg.vmux, cfg.level, cfg.sixdb ? "on" : "off",
        tenbit ? "on" : "off", cfg.center_offset, chunk_bytes >> 20);
    fflush(stdout);

    uint8_t* buf = (uint8_t*)malloc(chunk_bytes);
    if (!buf) {
        fprintf(stderr, "cannot allocate %zu bytes\n", chunk_bytes);
        CloseHandle(dev);
        return EXIT_FAILURE;
    }

    LARGE_INTEGER freq, t1, t2;
    QueryPerformanceFrequency(&freq);

    long line = 0;
    int rc = EXIT_SUCCESS;

    while (!g_stop && (max_lines == 0 || line < max_lines)) {
        DWORD got = 0;
        QueryPerformanceCounter(&t1);
        if (!ReadFile(dev, buf, (DWORD)chunk_bytes, &got, NULL)) {
            fprintf(stderr, "read failed: %s\n", win32_err(GetLastError()));
            rc = EXIT_FAILURE;
            break;
        }
        QueryPerformanceCounter(&t2);
        double secs = (double)(t2.QuadPart - t1.QuadPart) / (double)freq.QuadPart;

        if (got == 0)
            continue;

        size_t nsamp = tenbit ? got / 2 : got;
        const uint16_t* wbuf = (const uint16_t*)buf;

        uint64_t lo_sum = 0, hi_sum = 0, sum = 0;
        uint32_t lo_cnt = 0, hi_cnt = 0, clip_lo = 0, clip_hi = 0;
        unsigned low = maxv, high = minv;

        for (size_t i = 0; i < nsamp; i++) {
            unsigned v = tenbit ? (unsigned)(wbuf[i] >> 6) : (unsigned)buf[i];
            v += 1;
            sum += v;
            if (v < low) low = v;
            if (v > high) high = v;
            if (v < center) { lo_sum += v; lo_cnt++; } else { hi_sum += v; hi_cnt++; }
            if (v == minv) clip_lo++;
            if (v == maxv) clip_hi++;
        }

        ULONG ouflow = 0;
        if (!DeviceIoControl(dev, CX_IOCTL_STATE_OUFLOW_GET, NULL, 0, &ouflow, sizeof(ouflow), &ret_len, NULL))
            ouflow = (ULONG)-1;

        double rate = secs > 0 ? (double)nsamp / secs / 1.0e6 : 0.0;
        double avg_lo = lo_cnt ? (double)lo_sum / maxv / lo_cnt * 100.0 : 0.0;
        double avg_hi = hi_cnt ? (double)hi_sum / maxv / hi_cnt * 100.0 : 0.0;
        double dc = (double)sum / (double)nsamp / maxv * 100.0 - 50.0;

        printf("lo |%u| [%7.3f%%] (%7.3f%%) center %+.2f%% hi (%7.3f%%) [%7.3f%%] |%u|  nsamp %zu  rate %.2f  ouflow %lu%s\n",
            clip_lo, low * 100.0 / maxv, avg_lo, dc, avg_hi, high * 100.0 / maxv, clip_hi,
            nsamp, rate, ouflow, (clip_lo || clip_hi) ? "  CLIPPING" : "");
        fflush(stdout);
        line++;
    }

    free(buf);
    CloseHandle(dev);
    return rc;
}
