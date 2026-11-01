// xenative-ctl - drive the XeNative kext from user space.
//
//   xenative-ctl status            show live registers + decision state
//   xenative-ctl scan [limit]      count GGTT entries, show the live surface run
//   xenative-ctl switch            copy pattern in, point the plane at it
//   xenative-ctl restore           put the five plane registers back
//
// Requires root for the IOKit connection to an unentitled user client service.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

#define SEL_STATUS   0
#define SEL_SCAN     1
#define SEL_SWITCH   2
#define SEL_RESTORE  3
#define SEL_FILL     4
#define SEL_PTE      5
#define SEL_REG      6
#define SEL_SWITCHREL 7
#define SEL_FILLAT   8
#define SEL_FLUSH    9
#define SEL_BUF      10
#define SEL_WHERE    11
#define SEL_PEEK     12
#define SEL_BUFVIA   13
#define SEL_BUFLOW   14
#define SEL_MAPFAST  15
#define SEL_APFILL   16
#define SEL_REGSET   17
#define SEL_REGREAD  18
#define SEL_APSOLID  19
#define SEL_LATCH    20

static io_connect_t openConn(io_service_t *svcOut)
{
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("XeNative"));
    if (!svc) { fprintf(stderr, "XeNative service not found\n"); return 0; }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed: 0x%x\n", kr);
        IOObjectRelease(svc);
        return 0;
    }
    *svcOut = svc;
    return conn;
}

static int runN(uint32_t sel, const uint64_t *in, int nIn, uint64_t *out, uint32_t *outCount)
{
    io_service_t svc = 0;
    io_connect_t conn = openConn(&svc);
    if (!conn) return 1;
    kern_return_t kr = IOConnectCallScalarMethod(conn, sel,
                          in, (uint32_t)nIn, out, outCount);
    IOServiceClose(conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "call %u failed: 0x%x\n", sel, kr);
        return 2;
    }
    return 0;
}

static int run(uint32_t sel, uint64_t in, int haveIn, uint64_t *out, uint32_t *outCount)
{
    return runN(sel, &in, haveIn ? 1 : 0, out, outCount);
}

int main(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "status";
    uint64_t out[32];
    uint32_t n = 32;

    if (!strcmp(cmd, "status")) {
        if (run(SEL_STATUS, 0, 0, out, &n)) return 1;
        printf("magic            0x%llx\n", out[0]);
        printf("switched         %llu\n", out[1]);
        printf("gtt_mapped       %llu\n", out[2]);
        printf("gtt_entries      %llu\n", out[3]);
        printf("live_pte         0x%llx\n", out[4]);
        printf("plan_offset      0x%llx\n", out[5]);
        printf("SURF             0x%08llx\n", out[6] & 0xffffffffULL);
        printf("SURFLIVE         0x%08llx\n", out[7] & 0xffffffffULL);
        printf("PS_CTRL          0x%08llx\n", out[8] & 0xffffffffULL);
        printf("PIPESRC          0x%08llx\n", out[9] & 0xffffffffULL);
        printf("PLANE_SIZE       0x%08llx\n", out[10] & 0xffffffffULL);
        printf("PLANE_STRIDE     %llu\n", out[11] & 0xffffffffULL);
        printf("PLANE_CTL        0x%08llx\n", out[12] & 0xffffffffULL);
        printf("SURF_before      0x%08llx\n", out[13] & 0xffffffffULL);
        printf("phys_base        0x%llx\n", out[14]);
        printf("FRMCOUNT         %llu\n", out[15] & 0xffffffffULL);
    } else if (!strcmp(cmd, "scan")) {
        uint64_t limit = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x100000;
        if (run(SEL_SCAN, limit, 1, out, &n)) return 1;
        printf("nonzero_entries  %llu\n", out[0]);
        printf("present_entries  %llu\n", out[1]);
        printf("live_run_pages   %llu\n", out[2]);
        printf("live_phys        0x%llx\n", out[3]);
        printf("ptes_around_live:\n");
        for (unsigned k = 0; k < 12; k++)
            printf("  [liveIdx%+d] 0x%016llx\n", (int)k - 1, out[4 + k]);
    } else if (!strcmp(cmd, "switch")) {
        if (run(SEL_SWITCH, 0, 0, out, &n)) return 1;
        printf("SURF             0x%08llx\n", out[0] & 0xffffffffULL);
        printf("SURFLIVE         0x%08llx\n", out[1] & 0xffffffffULL);
        printf("PS_CTRL          0x%08llx\n", out[2] & 0xffffffffULL);
        printf("PIPESRC          0x%08llx\n", out[3] & 0xffffffffULL);
        printf("PLANE_SIZE       0x%08llx\n", out[4] & 0xffffffffULL);
        printf("PLANE_STRIDE     %llu\n", out[5] & 0xffffffffULL);
    } else if (!strcmp(cmd, "restore")) {
        if (run(SEL_RESTORE, 0, 0, out, &n)) return 1;
        printf("SURF             0x%08llx\n", out[0] & 0xffffffffULL);
        printf("SURFLIVE         0x%08llx\n", out[1] & 0xffffffffULL);
        printf("PS_CTRL          0x%08llx\n", out[2] & 0xffffffffULL);
        printf("PIPESRC          0x%08llx\n", out[3] & 0xffffffffULL);
        printf("PLANE_SIZE       0x%08llx\n", out[4] & 0xffffffffULL);
        printf("PLANE_STRIDE     %llu\n", out[5] & 0xffffffffULL);
    } else if (!strcmp(cmd, "fill")) {
        uint64_t wb = (argc > 2) ? strtoull(argv[2], NULL, 0) : 1;
        if (run(SEL_FILL, wb, 1, out, &n)) return 1;
        printf("filled           %llu  phys_base 0x%llx  flushes %llu\n", out[0], out[1], out[2]);
    } else if (!strcmp(cmd, "fillat")) {
        uint64_t in[2], phys = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;
        in[0] = phys;
        in[1] = (argc > 3) ? strtoull(argv[3], NULL, 0) : 1;
        if (runN(SEL_FILLAT, in, 2, out, &n)) return 1;
        printf("filled           %llu  at phys 0x%llx  flushes %llu\n", out[0], out[1], out[2]);
    } else if (!strcmp(cmd, "flush")) {
        if (run(SEL_FLUSH, 0, 0, out, &n)) return 1;
        printf("flush            ok  count %llu\n", out[1]);
    } else if (!strcmp(cmd, "pte")) {
        uint64_t in[3];
        if (argc < 4) { fprintf(stderr, "pte <index> <value> [wbinvd 0|1]\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = strtoull(argv[3], NULL, 0);
        in[2] = (argc > 4) ? strtoull(argv[4], NULL, 0) : 1;
        if (runN(SEL_PTE, in, 3, out, &n)) return 1;
        printf("pte[0x%llx] = 0x%016llx  (wbinvd %llu, flushes %llu)\n",
               in[0], out[0], in[2], out[1]);
    } else if (!strcmp(cmd, "map")) {
        // map <startIdx> <count> <physBase> [wbinvd] - one connection, N PTE writes
        if (argc < 5) { fprintf(stderr, "map <startIdx> <count> <physBase> [wb]\n"); return 2; }
        uint64_t i0 = strtoull(argv[2], NULL, 0);
        uint64_t cnt = strtoull(argv[3], NULL, 0);
        uint64_t pb = strtoull(argv[4], NULL, 0);
        uint64_t wb = (argc > 5) ? strtoull(argv[5], NULL, 0) : 0;
        io_service_t svc = 0; io_connect_t conn = openConn(&svc);
        if (!conn) return 1;
        uint64_t fail = 0;
        for (uint64_t k = 0; k < cnt; k++) {
            uint64_t in[3];
            in[0] = i0 + k;
            in[1] = ((pb + k * 4096ULL) & ~0xFFFULL) | 0x3ULL;
            in[2] = wb;
            uint32_t m = 8;
            if (IOConnectCallScalarMethod(conn, SEL_PTE, in, 3, out, &m) != KERN_SUCCESS) { fail++; break; }
            if (out[0] != in[1]) { fail++; break; }
        }
        IOServiceClose(conn); IOObjectRelease(svc);
        printf("mapped           0x%llx .. 0x%llx -> phys 0x%llx  failures %llu\n",
               i0, i0 + cnt - 1, pb, fail);
        return fail ? 1 : 0;
    } else if (!strcmp(cmd, "reg")) {
        uint64_t in[2];
        if (argc < 4) { fprintf(stderr, "reg <0 surf|1 size|2 stride|3 pipesrc|4 psctrl|5 planectl> <value>\n"); return 2; }
        io_service_t svc = 0; io_connect_t conn = openConn(&svc);
        if (!conn) return 1;
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = strtoull(argv[3], NULL, 0);
        kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_REG, in, 2, out, &n);
        IOServiceClose(conn); IOObjectRelease(svc);
        if (kr) { fprintf(stderr, "reg failed 0x%x\n", kr); return 1; }
        printf("reg  = 0x%08llx  SURFLIVE = 0x%08llx\n",
               out[0] & 0xffffffffULL, out[1] & 0xffffffffULL);
    } else if (!strcmp(cmd, "switchrel")) {
        if (run(SEL_SWITCHREL, 0, 0, out, &n)) return 1;
        printf("SURF             0x%08llx\n", out[0] & 0xffffffffULL);
        printf("SURFLIVE         0x%08llx\n", out[1] & 0xffffffffULL);
        printf("PS_CTRL          0x%08llx\n", out[2] & 0xffffffffULL);
        printf("PIPESRC          0x%08llx\n", out[3] & 0xffffffffULL);
        printf("PLANE_SIZE       0x%08llx\n", out[4] & 0xffffffffULL);
        printf("PLANE_STRIDE     %llu\n", out[5] & 0xffffffffULL);
    } else if (!strcmp(cmd, "buf1080") || !strcmp(cmd, "buf3120")) {
        uint64_t mode = (cmd[3] == '1') ? 0 : 1;
        uint64_t in[2];
        in[0] = mode;
        in[1] = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;   // optional physical mask
        if (runN(SEL_BUF, in, 2, out, &n)) return 1;
        printf("verify           %s (%llu bad)\n", out[0] ? "ok" : "MISMATCH", out[9]);
        printf("surf             0x%08llx  (%llu x %llu pattern, %llu pages)\n",
               out[1] & 0xffffffffULL, out[10], out[11], out[2]);
        printf("buf_phys0        0x%llx   max 0x%llx\n", out[3], out[15]);
        printf("phys readback    [0]=0x%08llx [+64]=0x%08llx [+1M]=0x%08llx\n",
               out[5] & 0xffffffffULL, out[6] & 0xffffffffULL, out[7] & 0xffffffffULL);
        printf("pattern sum      0x%llx\n", out[8]);
        printf("pattern px       [0]=0x%08llx mid=0x%08llx last=0x%08llx\n",
               out[12] & 0xffffffffULL, out[13] & 0xffffffffULL, out[14] & 0xffffffffULL);
    } else if (!strcmp(cmd, "bufvia")) {
        if (run(SEL_BUFVIA, 0, 0, out, &n)) return 1;
        printf("verify           %s (%llu bad)\n", out[0] ? "ok" : "MISMATCH", out[5]);
        printf("surf             0x%08llx  pages %llu\n", out[1] & 0xffffffffULL, out[2]);
        printf("buf_phys0        0x%llx   phys readback [0]=0x%08llx\n",
               out[3], out[4] & 0xffffffffULL);
        printf("pattern sum      0x%llx  px[0]=0x%08llx\n", out[6], out[7] & 0xffffffffULL);
    } else if (!strcmp(cmd, "buflow")) {
        // buflow [mask] [tries]
        uint64_t in[2];
        in[0] = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0;
        in[1] = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;
        if (runN(SEL_BUFLOW, in, 2, out, &n)) {
            fprintf(stderr, "no low buffer; best phys tried 0x%llx\n", out[3]);
            return 1;
        }
        printf("verify           %s (%llu bad)\n", out[0] ? "ok" : "MISMATCH", out[5]);
        printf("surf             0x%08llx  pages %llu\n", out[1] & 0xffffffffULL, out[2]);
        printf("buf_phys0        0x%llx   max 0x%llx   mask 0x%llx\n",
               out[3], out[6], out[7]);
        printf("phys readback    [0]=0x%08llx\n", out[4] & 0xffffffffULL);
    } else if (!strcmp(cmd, "mapfast")) {
        // mapfast <idx> <count> <phys>
        uint64_t in[3];
        if (argc < 5) { fprintf(stderr, "mapfast <idx> <count> <phys>\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = strtoull(argv[3], NULL, 0);
        in[2] = strtoull(argv[4], NULL, 0);
        n = 32;
        if (runN(SEL_MAPFAST, in, 3, out, &n)) return 1;
        printf("mapped   0x%llx .. 0x%llx  first 0x%016llx last 0x%016llx  bad %llu\n",
               out[0], out[0] + out[1] - 1, out[2], out[3], out[4]);
    } else if (!strcmp(cmd, "apfill")) {
        // apfill <idx> [0=1080|1=3120]
        uint64_t in[2];
        if (argc < 3) { fprintf(stderr, "apfill <idx> [mode]\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;
        n = 32;
        if (runN(SEL_APFILL, in, 2, out, &n)) return 1;
        printf("apfill           %s  aperture %llu  need_end 0x%llx\n",
               out[0] ? "ok" : "FAILED", out[1], out[2]);
        printf("px seen via MMIO [0]=0x%08llx mid=0x%08llx  sum 0x%llx\n",
               out[3] & 0xffffffffULL, out[4] & 0xffffffffULL, out[5]);
    } else if (!strcmp(cmd, "regset")) {
        // regset <n> <which> <value> [<which> <value> ...]  - all writes back to back
        uint64_t in[17];
        if (argc < 5) {
            fprintf(stderr, "regset <n> <which> <value> ...\n"
                            "  which: 0 surf|1 size|2 stride|3 pipesrc|4 psctrl|5 planectl\n");
            return 2;
        }
        unsigned nPairs = (unsigned)strtoul(argv[2], NULL, 0);
        if (!nPairs || nPairs > 8 || (unsigned)argc < 3 + 2 * nPairs) {
            fprintf(stderr, "regset: bad pair count\n");
            return 2;
        }
        in[0] = nPairs;
        for (unsigned i = 0; i < nPairs; i++) {
            in[1 + i * 2] = strtoull(argv[3 + i * 2], NULL, 0);
            in[2 + i * 2] = strtoull(argv[4 + i * 2], NULL, 0);
        }
        n = 32;
        if (runN(SEL_REGSET, in, 1 + 2 * (int)nPairs, out, &n)) return 1;
        for (unsigned i = 0; i < nPairs; i++)
            printf("reg[%llu] wrote 0x%08llx  read 0x%08llx\n",
                   out[i * 2], in[2 + i * 2] & 0xffffffffULL, out[i * 2 + 1] & 0xffffffffULL);
        printf("SURFLIVE         0x%08llx\n", out[nPairs * 2] & 0xffffffffULL);
        printf("PS_CTRL          0x%08llx\n", out[nPairs * 2 + 1] & 0xffffffffULL);
    } else if (!strcmp(cmd, "dump")) {
        // dump <offset> [count]  - read-only raw BAR0 dwords
        uint64_t in[2];
        if (argc < 3) { fprintf(stderr, "dump <offset> [count] [n]\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = (argc > 3) ? strtoull(argv[3], NULL, 0) : 4;
        n = 32;
        if (runN(SEL_REGREAD, in, 2, out, &n)) return 1;
        for (uint32_t i = 0; i < in[1] && i < n; i++)
            printf("  0x%06llx = 0x%08llx\n", in[0] + i * 4, out[i] & 0xffffffffULL);
    } else if (!strcmp(cmd, "apcolor")) {
        // apcolor <idx> <0xAARRGGBB> [0=1080|1=3120]
        uint64_t in[3];
        if (argc < 4) { fprintf(stderr, "apcolor <idx> <colour> [mode]\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = strtoull(argv[3], NULL, 0);
        in[2] = (argc > 4) ? strtoull(argv[4], NULL, 0) : 0;
        n = 32;
        if (runN(SEL_APSOLID, in, 3, out, &n)) return 1;
        printf("apcolor          %s  px[0]=0x%08llx mid=0x%08llx\n",
               out[0] ? "ok" : "FAILED", out[1] & 0xffffffffULL, out[2] & 0xffffffffULL);
    } else if (!strcmp(cmd, "latch")) {
        // latch - force a plane register latch (geometry is double buffered)
        if (run(SEL_LATCH, 0, 0, out, &n)) return 1;
        printf("latch            SURF 0x%08llx  SURFLIVE 0x%08llx\n",
               out[0] & 0xffffffffULL, out[1] & 0xffffffffULL);
    } else if (!strcmp(cmd, "peek")) {
        // peek <phys> [count]
        uint64_t in[2];
        if (argc < 3) { fprintf(stderr, "peek <phys> [count]\n"); return 2; }
        in[0] = strtoull(argv[2], NULL, 0);
        in[1] = (argc > 3) ? strtoull(argv[3], NULL, 0) : 8;
        n = 32;
        if (runN(SEL_PEEK, in, 2, out, &n)) return 1;
        for (uint32_t i = 0; i < in[1] && i < n; i++)
            printf("  [0x%llx] 0x%08llx\n", in[0] + i * 4, out[i] & 0xffffffffULL);
    } else if (!strcmp(cmd, "where")) {
        if (run(SEL_WHERE, 0, 0, out, &n)) return 1;
        printf("buf_phys0        0x%llx\n", out[0]);
        printf("buf_pages        %llu\n", out[1]);
        printf("buf_phys_max     0x%llx\n", out[14]);
        printf("pattern          %llu x %llu   sum 0x%llx\n", out[2], out[3], out[6]);
        printf("pattern px       [0]=0x%08llx mid=0x%08llx last=0x%08llx\n",
               out[7] & 0xffffffffULL, out[8] & 0xffffffffULL, out[9] & 0xffffffffULL);
        printf("mapped           %llu pages at surf 0x%08llx\n", out[5], out[4] & 0xffffffffULL);
        printf("pte first/mid/last 0x%016llx 0x%016llx 0x%016llx  (bad %llu)\n",
               out[10], out[11], out[12], out[13]);
        printf("flushes          %llu\n", out[15]);
    } else {
        fprintf(stderr, "usage: %s status|scan [limit]|switch|switchrel|restore|fill [wb]|fillat <phys> [wb]|pte <i> <v> [wb]|reg <n> <v>|regset <n> <which> <val> ...|latch|dump <off> [n]|apcolor <idx> <col> [mode]|flush|buf1080|buf3120|where|regset\n", argv[0]);
        return 2;
    }
    return 0;
}
