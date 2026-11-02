// h64hdconv: converts a community N64 hi-res texture pack (GLideN64 / Rice
// PNG names: NAME#TEXCRC#FMT#SIZE[#PALCRC]_suffix.png) into a Harissa64 HD
// file (core/rdp/h64_hdpack.h) the Xbox 360 front end reads.
//
//   h64hdconv PACK ROM_NAME OUT.h64hd [--max N] [--threads N]
//
// PACK: the pack's .zip as downloaded, or its extracted folder (searched
// recursively). ROM_NAME: the ROM's internal name as the PNG names start
// ("SUPER MARIO 64"; the case is ignored). --max N: textures larger than N
// pixels on a side are halved until they fit (default 1024). Pictures in
// "Hacks/", "iQue/", "Ports/" and "rt64/" folders (other ROMs, other
// emulators) are left out. The work is shared by --threads threads (default:
// every processor).
//
// The conversion itself is core/rdp/h64_hdbuild.cpp, which the console uses too.
#include "../../core/cart/h64_zip.h"
#include "../../core/rdp/h64_hdbuild.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct Source
{
    std::string path;     // a file of the folder, or the entry's name in the zip
    H64ZipEntry entry;    // (zip)
    H64HdEntry key;
};

struct ZipFile { FILE *f; std::mutex lock; };

static u32 zip_read(void *user, u32 offset, void *buf, u32 len)
{
    ZipFile *z = (ZipFile *)user;
    std::lock_guard<std::mutex> g(z->lock);
#ifdef _WIN32
    if (_fseeki64(z->f, (long long)offset, SEEK_SET)) return 0;
#else
    if (fseeko(z->f, (off_t)offset, SEEK_SET)) return 0;
#endif
    return (u32)fread(buf, 1, len, z->f);
}

struct ListCtx { std::vector<Source> *out; const char *rom; };

static int zip_list(void *user, const H64ZipEntry *e)
{
    ListCtx *c = (ListCtx *)user;
    const char *slash = strrchr(e->name, '/'), *file = slash ? slash + 1 : e->name;
    size_t n = strlen(file);
    Source s;
    if (n < 4 || (strcmp(file + n - 4, ".png") && strcmp(file + n - 4, ".PNG"))) return 0;
    if (h64_hdbuild_skip(e->name) || !h64_hdbuild_parse(file, c->rom, &s.key)) return 0;
    s.path = e->name;
    s.entry = *e;
    c->out->push_back(s);
    return 0;
}

static bool read_file(const std::string &path, std::vector<u8> &data)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: h64hdconv PACK(.zip or folder) ROM_NAME OUT.h64hd [--max N] [--threads N]\n");
        return 2;
    }
    std::string pack = argv[1], rom = argv[2], outPath = argv[3];
    int maxSide = 1024, threads = (int)std::thread::hardware_concurrency();
    for (int i = 4; i < argc; i++)
    {
        if (!strcmp(argv[i], "--max") && i + 1 < argc) maxSide = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
    }
    if (threads < 1) threads = 1;

    // The pictures of this ROM in the pack.
    std::vector<Source> list;
    ZipFile zf;
    H64ZipReader zr;
    bool isZip = fs::is_regular_file(pack);
    if (isZip)
    {
        zf.f = fopen(pack.c_str(), "rb");
        if (!zf.f) { fprintf(stderr, "cannot open %s\n", pack.c_str()); return 1; }
        u64 size = fs::file_size(pack);
        if (size >= 0xFFFFFFFFull) { fprintf(stderr, "%s: archives of 4 GB or more are not supported\n", pack.c_str()); return 1; }
        zr.user = &zf;
        zr.size = (u32)size;
        zr.read = zip_read;
        ListCtx c = { &list, rom.c_str() };
        if (h64_zip_each(&zr, zip_list, &c) < 0) { fprintf(stderr, "%s is not a zip archive\n", pack.c_str()); return 1; }
    }
    else
    {
        for (const fs::directory_entry &de : fs::recursive_directory_iterator(pack))
        {
            if (!de.is_regular_file()) continue;
            std::string path = de.path().generic_string(), name = de.path().filename().string();
            if (name.size() < 4 || (name.compare(name.size() - 4, 4, ".png") && name.compare(name.size() - 4, 4, ".PNG"))) continue;
            Source s;
            if (h64_hdbuild_skip(path.c_str()) || !h64_hdbuild_parse(name.c_str(), rom.c_str(), &s.key)) continue;
            s.path = path;
            list.push_back(s);
        }
    }
    if (list.empty()) { fprintf(stderr, "no picture of \"%s\" in %s\n", rom.c_str(), pack.c_str()); return 1; }

    H64HdBuilder *b = h64_hdbuild_begin(outPath.c_str(), (u32)list.size());
    if (!b) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    std::atomic<size_t> next(0), done(0), failed(0);
    std::atomic<bool> writeError(false);
    std::mutex outLock;
    auto work = [&]() {
        std::vector<u8> png;
        for (;;)
        {
            size_t i = next++;
            if (i >= list.size()) break;
            const Source &s = list[i];
            bool ok;
            if (isZip)
            {
                png.resize(s.entry.size);
                ok = s.entry.size && h64_zip_extract(&zr, &s.entry, png.data(), s.entry.size) == 0;
            }
            else
                ok = read_file(s.path, png);
            H64HdItem item;
            item.e = s.key;
            item.e.pad2 = (u32)i;   // ties between repeated keys: the first in the pack's order
            if (!ok || h64_hdbuild_make(png.data(), (u32)png.size(), maxSide, &item))
            {
                failed++;
                fprintf(stderr, "cannot read %s\n", s.path.c_str());
                continue;
            }
            {
                std::lock_guard<std::mutex> g(outLock);
                if (h64_hdbuild_add(b, &item)) writeError = true;
            }
            size_t d = ++done;
            if (d % 500 == 0) fprintf(stderr, "%zu / %zu textures...\n", d, list.size());
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) pool.push_back(std::thread(work));
    for (size_t t = 0; t < pool.size(); t++) pool[t].join();
    if (isZip) fclose(zf.f);
    if (writeError) { h64_hdbuild_abort(b); fprintf(stderr, "cannot write %s (disk full, or over 2 GB)\n", outPath.c_str()); return 1; }
    u32 count, repeated, bytes;
    if (h64_hdbuild_end(b, &count, &repeated, &bytes)) { fprintf(stderr, "cannot write %s\n", outPath.c_str()); return 1; }
    printf("%s: %u textures (%zu PNG names, %u repeated, %zu unreadable), %.1f MB\n", outPath.c_str(), count, list.size(), repeated,
           (size_t)failed, bytes / 1048576.0);
    return 0;
}
