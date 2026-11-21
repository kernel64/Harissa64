// Harissa64 V2 - rdp-oracle: plays an RDPDUMP2 file with ParaLLEl-RDP and
// Angrylion-Plus and writes the RDRAM each leaves behind, for the render
// tests (h64test --rt-compare). Test tool for the PC only.
//
// RDPDUMP2 (ParaLLEl-RDP's rdp_dump_write.cpp / rdp_dump.cpp, MIT): "RDPDUMP2",
// RDRAM size, hidden RDRAM size, then records of 32-bit host words:
//   1 UpdateDram offset size bytes   2 RDPCommand opcode count words
//   3 SetVIRegister index value      4 EndFrame   5 SignalComplete   6 EndOfFile
//   7 UpdateDramFlush                8 UpdateHiddenDram offset size bytes   9 UpdateHiddenDramFlush
// Our scenes (platform/host/h64_rtdump.h) give RDRAM changes applied at once,
// not ParaLLEl's full-memory flushes, and record 0x48 (skipped here).
// RDRAM bytes are in the host's 32-bit word order (on a little-endian PC, N64
// byte address A is byte A ^ 3), as both renderers keep RDRAM; the hidden
// RDRAM is one byte per halfword, indexed by N64 halfword address. The files
// this tool writes are in N64 byte order (as h64test's RDRAM).
//
// Usage: rdp-oracle DUMP OUTDIR [--frames all|last|N,N,...] [--only parallel|angrylion]
// Writes OUTDIR/<renderer>_f<N>.rdram (RDRAM) and .hidden after frame N
// (counted from 0 by EndFrame records; "last" = at the end of the file).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "rdp_device.hpp"
#include "aligned_alloc.hpp"

extern "C" {
#include "n64video.h"
#include "msg.h"
#include "vdac.h"
void rdp_cmd(uint32_t wid, const uint32_t *args);
extern uint8_t rdram_hidden[RDRAM_MAX_SIZE / 2];
}

// ---- Angrylion's platform hooks: no window, messages to stderr.
extern "C" {
void vdac_init(struct n64video_config *) {}
void vdac_write(struct frame_buffer *) {}
void vdac_sync(bool) {}
void vdac_close(void) {}
static void vmsg(const char *kind, const char *fmt, va_list va)
{
    fprintf(stderr, "[angrylion %s] ", kind);
    vfprintf(stderr, fmt, va);
    fputc('\n', stderr);
}
void msg_error(const char *fmt, ...) { va_list va; va_start(va, fmt); vmsg("error", fmt, va); va_end(va); }
void msg_warning(const char *fmt, ...) { va_list va; va_start(va, fmt); vmsg("warning", fmt, va); va_end(va); }
void msg_debug(const char *, ...) {}
}

struct Renderer
{
    virtual ~Renderer() {}
    virtual const char *name() const = 0;
    // Bytes at `offset` (host word order); h64_rtdump.h: records are applied at once.
    virtual void rdram(size_t offset, const uint8_t *data, size_t size) = 0;
    virtual void hidden(size_t offset, const uint8_t *data, size_t size) = 0;
    virtual void command(uint32_t count, const uint32_t *words) = 0;
    virtual void vi(uint32_t reg, uint32_t value) = 0;
    virtual void sync() = 0;
    virtual const uint8_t *read_rdram() = 0;
    virtual const uint8_t *read_hidden() = 0;
};

struct Angrylion : Renderer
{
    std::vector<uint8_t> ram;
    uint32_t viRegs[VI_NUM_REG], dpRegs[DP_NUM_REG], irq;
    uint32_t *pVi[VI_NUM_REG], *pDp[DP_NUM_REG];
    n64video_config config;

    explicit Angrylion(size_t size) : ram(size)
    {
        unsigned i;
        memset(viRegs, 0, sizeof(viRegs));
        memset(dpRegs, 0, sizeof(dpRegs));
        irq = 0;
        for (i = 0; i < VI_NUM_REG; i++) pVi[i] = &viRegs[i];
        for (i = 0; i < DP_NUM_REG; i++) pDp[i] = &dpRegs[i];
        memset(&config, 0, sizeof(config));   // serial, as ParaLLEl's conformance harness runs it
        config.gfx.rdram = ram.data();
        config.gfx.rdram_size = (uint32_t)ram.size();
        config.gfx.vi_reg = pVi;
        config.gfx.dp_reg = pDp;
        config.gfx.mi_intr_reg = &irq;
        config.gfx.mi_intr_cb = noop;
        config.vi.mode = VI_MODE_NORMAL;
        config.vi.interp = VI_INTERP_LINEAR;
        config.dp.compat = DP_COMPAT_HIGH;
        n64video_init(&config);
    }
    ~Angrylion() { n64video_close(); }
    static void noop() {}
    const char *name() const { return "angrylion"; }
    void rdram(size_t o, const uint8_t *d, size_t n) { memcpy(ram.data() + o, d, n); }
    void hidden(size_t o, const uint8_t *d, size_t n) { memcpy(rdram_hidden + o, d, n); }
    void command(uint32_t, const uint32_t *w) { rdp_cmd(0, w); }
    void vi(uint32_t reg, uint32_t value) { if (reg < VI_NUM_REG) viRegs[reg] = value; }
    void sync() {}
    const uint8_t *read_rdram() { return ram.data(); }
    const uint8_t *read_hidden() { return rdram_hidden; }
};

struct Parallel : Renderer
{
    Vulkan::Context ctx;
    Vulkan::Device dev;
    void *host;
    size_t size;
    std::unique_ptr<RDP::CommandProcessor> gpu;

    Parallel() : host(0), size(0) {}
    bool init(size_t rdramSize, size_t hiddenSize)
    {
        if (!Vulkan::Context::init_loader(nullptr)) { fprintf(stderr, "no Vulkan loader\n"); return false; }
        if (!ctx.init_instance_and_device(nullptr, 0, nullptr, 0)) { fprintf(stderr, "no Vulkan device\n"); return false; }
        dev.set_context(ctx);
        size = rdramSize;
        host = Util::memalign_calloc(64 * 1024, size);
        gpu.reset(new RDP::CommandProcessor(dev, host, 0, size, hiddenSize,
                                            RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_HIDDEN_RDRAM_BIT |
                                            RDP::COMMAND_PROCESSOR_FLAG_HOST_VISIBLE_TMEM_BIT));
        if (!gpu->device_is_supported()) { fprintf(stderr, "ParaLLEl-RDP: GPU not supported\n"); return false; }
        return true;
    }
    ~Parallel()
    {
        if (gpu) gpu->idle();
        gpu.reset();
        if (host) Util::memalign_free(host);
    }
    const char *name() const { return "parallel"; }
    void rdram(size_t o, const uint8_t *d, size_t n)
    {
        gpu->idle();
        gpu->begin_read_rdram();
        memcpy((uint8_t *)host + o, d, n);
        gpu->end_write_rdram();
    }
    void hidden(size_t o, const uint8_t *d, size_t n)
    {
        gpu->idle();
        memcpy((uint8_t *)gpu->begin_read_hidden_rdram() + o, d, n);
        gpu->end_write_hidden_rdram();
    }
    void command(uint32_t n, const uint32_t *w) { gpu->enqueue_command(n, w); }
    void vi(uint32_t reg, uint32_t value) { gpu->set_vi_register(RDP::VIRegister(reg), value); }
    void sync() { gpu->flush(); }
    const uint8_t *read_rdram()
    {
        gpu->idle();
        gpu->begin_read_rdram();
        return (const uint8_t *)host;
    }
    const uint8_t *read_hidden()
    {
        gpu->idle();
        return (const uint8_t *)gpu->begin_read_hidden_rdram();
    }
};

static bool rd(FILE *f, uint32_t &v) { return fread(&v, 4, 1, f) == 1; }

static bool write_file(const std::string &path, const uint8_t *data, size_t size, bool swapWords)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return false; }
    if (swapWords)
    {
        std::vector<uint8_t> tmp(size);
        for (size_t i = 0; i < size; i += 4)
        {
            tmp[i] = data[i + 3]; tmp[i + 1] = data[i + 2]; tmp[i + 2] = data[i + 1]; tmp[i + 3] = data[i];
        }
        fwrite(tmp.data(), 1, size, f);
    }
    else
        fwrite(data, 1, size, f);
    fclose(f);
    return true;
}

static void snapshot(std::vector<Renderer *> &rs, const std::string &out, const std::string &tag, size_t ramSize, size_t hidSize)
{
    for (size_t k = 0; k < rs.size(); k++)
    {
        std::string base = out + "/" + rs[k]->name() + "_" + tag;
        write_file(base + ".rdram", rs[k]->read_rdram(), ramSize, true);
        write_file(base + ".hidden", rs[k]->read_hidden(), hidSize, false);
    }
    printf("[oracle] wrote %s\n", tag.c_str());
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: rdp-oracle DUMP OUTDIR [--frames all|last|N,N,...] [--only parallel|angrylion]\n");
        return 3;
    }
    std::string dumpPath = argv[1], out = argv[2], only;
    std::set<uint32_t> frames;
    bool allFrames = false, last = true;
    for (int i = 3; i < argc; i++)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
        {
            const char *s = argv[++i];
            last = false;
            if (!strcmp(s, "all")) allFrames = true;
            else if (!strcmp(s, "last")) last = true;
            else
                while (*s)
                {
                    frames.insert((uint32_t)strtoul(s, (char **)&s, 10));
                    if (*s == ',') s++;
                    else break;
                }
        }
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
    }

    FILE *f = fopen(dumpPath.c_str(), "rb");
    char magic[8];
    uint32_t ramSize, hidSize;
    if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "RDPDUMP2", 8) || !rd(f, ramSize) || !rd(f, hidSize) ||
        (ramSize != 4u << 20 && ramSize != 8u << 20) || hidSize != 4u << 20)
    {
        fprintf(stderr, "not an RDPDUMP2 file: %s\n", dumpPath.c_str());
        return 3;
    }

    std::vector<Renderer *> rs;
    Angrylion *ang = 0;
    Parallel *par = 0;
    if (only.empty() || only == "angrylion") rs.push_back(ang = new Angrylion(ramSize));
    if (only.empty() || only == "parallel")
    {
        par = new Parallel;
        if (!par->init(ramSize, hidSize)) return 1;
        rs.push_back(par);
    }

    std::vector<uint8_t> ram(ramSize), hid(hidSize);
    std::vector<uint32_t> words;
    uint32_t frame = 0, commands = 0, syncs = 0, cmd;
    bool eof = false;
    while (!eof && rd(f, cmd))
    {
        uint32_t a, b;
        switch (cmd)
        {
        case 1: case 8:
            if (!rd(f, a) || !rd(f, b)) { eof = true; break; }
            {
                std::vector<uint8_t> &m = cmd == 1 ? ram : hid;
                if ((size_t)a + b > m.size() || fread(m.data() + a, 1, b, f) != b) { fprintf(stderr, "bad memory record\n"); return 1; }
                for (size_t k = 0; k < rs.size(); k++)
                    if (cmd == 1) rs[k]->rdram(a, m.data() + a, b);
                    else rs[k]->hidden(a, m.data() + a, b);
            }
            break;
        case 7: case 9: break;   // group markers
        case 0x48:   // HLE triangle (h64_rtdump.h): the RDP command after it is drawn
            for (int k = 0; k < 33; k++) if (!rd(f, a)) { eof = true; break; }
            break;
        case 2:
            if (!rd(f, a) || !rd(f, b) || b > 64) { fprintf(stderr, "bad command record\n"); return 1; }
            words.resize(b ? b : 1);
            if (b && fread(words.data(), 4, b, f) != b) { fprintf(stderr, "short command\n"); return 1; }
            for (size_t k = 0; k < rs.size(); k++) rs[k]->command(b, words.data());
            commands++;
            break;
        case 3:
            if (!rd(f, a) || !rd(f, b)) { eof = true; break; }
            for (size_t k = 0; k < rs.size(); k++) rs[k]->vi(a, b);
            break;
        case 5:
            for (size_t k = 0; k < rs.size(); k++) rs[k]->sync();
            syncs++;
            break;
        case 4:
            if (allFrames || frames.count(frame))
            {
                char tag[32];
                sprintf(tag, "f%u", frame);
                snapshot(rs, out, tag, ramSize, hidSize);
            }
            frame++;
            break;
        case 6: eof = true; break;
        default: fprintf(stderr, "unknown record %u\n", cmd); return 1;
        }
    }
    fclose(f);
    if (last) snapshot(rs, out, "last", ramSize, hidSize);
    printf("[oracle] %u frames, %u commands, %u syncs\n", frame, commands, syncs);
    for (size_t k = 0; k < rs.size(); k++) delete rs[k];
    return 0;
}
