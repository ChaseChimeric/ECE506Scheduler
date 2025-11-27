#include "fpga_loader_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

struct Options {
    bool show_help = false;
    bool dry_run = false;
    std::string static_bit = "bitstreams/top_reconfig_wrapper.bin";
    std::string partial_bit = "bitstreams/fft_partial.bin";
    std::string manager_node = "/sys/class/fpga_manager/fpga0/firmware";
    std::string firmware_dir = "/lib/firmware";
    uint64_t gpio_base = 0x41200000u;
    size_t gpio_span = 0x1000;
    std::chrono::milliseconds timeout{5000};

    std::string mm2s_buf = "/dev/udmabuf0";
    std::string s2mm_buf = "/dev/udmabuf1";
    uint64_t dma_base = 0x40400000u;
    size_t dma_span = 0x10000;
    size_t samples = 1024;
};

bool parse_u64(const std::string& text, uint64_t& value) {
    try {
        size_t idx = 0;
        value = std::stoull(text, &idx, 0);
        return idx == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_options(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            opts.show_help = true;
            return true;
        }
        if (arg == "--dry-run") {
            opts.dry_run = true;
            continue;
        }
        auto pos = arg.find('=');
        if (pos == std::string::npos) {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
        std::string key = arg.substr(0, pos);
        std::string value = arg.substr(pos + 1);
        if (key == "--static") {
            opts.static_bit = value;
        } else if (key == "--partial") {
            opts.partial_bit = value;
        } else if (key == "--manager") {
            opts.manager_node = value;
        } else if (key == "--firmware-dir") {
            opts.firmware_dir = value;
        } else if (key == "--gpio-base") {
            if (!parse_u64(value, opts.gpio_base)) {
                std::cerr << "Failed to parse --gpio-base value\n";
                return false;
            }
        } else if (key == "--gpio-span") {
            uint64_t span = 0;
            if (!parse_u64(value, span)) {
                std::cerr << "Failed to parse --gpio-span value\n";
                return false;
            }
            opts.gpio_span = static_cast<size_t>(span);
        } else if (key == "--wait-ms") {
            uint64_t ms = 0;
            if (!parse_u64(value, ms)) {
                std::cerr << "Failed to parse --wait-ms value\n";
                return false;
            }
            opts.timeout = std::chrono::milliseconds(ms);
        } else if (key == "--mm2s-buf") {
            opts.mm2s_buf = value;
        } else if (key == "--s2mm-buf") {
            opts.s2mm_buf = value;
        } else if (key == "--dma-base") {
            if (!parse_u64(value, opts.dma_base)) {
                std::cerr << "Failed to parse --dma-base value\n";
                return false;
            }
        } else if (key == "--dma-span") {
            uint64_t span = 0;
            if (!parse_u64(value, span)) {
                std::cerr << "Failed to parse --dma-span value\n";
                return false;
            }
            opts.dma_span = static_cast<size_t>(span);
        } else if (key == "--samples") {
            uint64_t count = 0;
            if (!parse_u64(value, count)) {
                std::cerr << "Failed to parse --samples value\n";
                return false;
            }
            opts.samples = static_cast<size_t>(count);
        } else {
            std::cerr << "Unknown option: " << key << "\n";
            return false;
        }
    }
    return true;
}

void print_usage() {
    std::cout << "Usage: fpga_fft_dma_loader [options]\n"
                 "\n"
                 "Loader options mirror fpga_loader plus DMA controls:\n"
                 "  --static=PATH           Static shell (.bin)\n"
                 "  --partial=PATH          FFT partial (.bin)\n"
                 "  --manager=PATH          fpga_manager firmware node\n"
                 "  --firmware-dir=DIR      Directory to stage bitstreams (/lib/firmware)\n"
                 "  --gpio-base=ADDR        AXI GPIO base for decouple (0x41200000)\n"
                 "  --gpio-span=BYTES       Span for GPIO mmap (0x1000)\n"
                 "  --mm2s-buf=/dev/...     u-dma-buf device feeding MM2S (udmabuf0)\n"
                 "  --s2mm-buf=/dev/...     u-dma-buf device for S2MM output (udmabuf1)\n"
                 "  --dma-base=ADDR         AXI DMA lite base (0x40400000)\n"
                 "  --dma-span=BYTES        Span when mapping DMA regs (0x10000)\n"
                 "  --samples=N             Number of 32-bit samples to transfer (1024)\n"
                 "  --wait-ms=MS            Timeout waiting for fpga_manager (5000)\n"
                 "  --dry-run               Skip hardware access, log actions\n"
                 "  -h, --help              Show this message\n";
}

bool read_u64_from_file(const std::string& path, uint64_t& value) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string text;
    std::getline(ifs, text);
    try {
        value = std::stoull(text, nullptr, 0);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct UdmaBuffer {
    std::string dev_path;
    std::string name;
    int fd = -1;
    uint8_t* virt = nullptr;
    size_t size = 0;
    uint64_t phys = 0;

    bool open(const std::string& path, bool dry_run) {
        dev_path = path;
        name = std::filesystem::path(path).filename().string();
        if (name.empty()) name = path;
        if (dry_run) return true;
        std::string base = "/sys/class/u-dma-buf/" + name;
        uint64_t tmp = 0;
        if (!read_u64_from_file(base + "/size", tmp)) {
            std::cerr << "[dma] Failed to read size for " << name << "\n";
            return false;
        }
        size = static_cast<size_t>(tmp);
        if (!read_u64_from_file(base + "/phys_addr", phys)) {
            std::cerr << "[dma] Failed to read phys_addr for " << name << "\n";
            return false;
        }
        fd = ::open(path.c_str(), O_RDWR | O_SYNC);
        if (fd < 0) {
            std::cerr << "[dma] Unable to open " << path << ": " << std::strerror(errno) << "\n";
            return false;
        }
        virt = static_cast<uint8_t*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (virt == MAP_FAILED) {
            std::cerr << "[dma] mmap failed for " << path << ": " << std::strerror(errno) << "\n";
            ::close(fd);
            fd = -1;
            virt = nullptr;
            return false;
        }
        return true;
    }

    void close(bool dry_run) {
        if (dry_run) return;
        if (virt && virt != MAP_FAILED) {
            ::munmap(virt, size);
            virt = nullptr;
        }
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

class MmioRegion {
public:
    bool open(uint64_t base, size_t span, bool dry_run) {
        dry_run_ = dry_run;
        if (dry_run_) return true;
        page_size_ = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        if (page_size_ == 0) page_size_ = 4096;
        off_t page_base = static_cast<off_t>(base & ~static_cast<uint64_t>(page_size_ - 1));
        size_t offset = static_cast<size_t>(base - static_cast<uint64_t>(page_base));
        map_len_ = ((offset + span + page_size_ - 1) / page_size_) * page_size_;
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "[dma] Failed to open /dev/mem: " << std::strerror(errno) << "\n";
            return false;
        }
        map_base_ = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, page_base);
        if (map_base_ == MAP_FAILED) {
            std::cerr << "[dma] mmap failed for DMA regs: " << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        regs_ = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(map_base_) + offset);
        return true;
    }

    void close() {
        if (dry_run_) return;
        if (map_base_ && map_base_ != MAP_FAILED) {
            ::munmap(map_base_, map_len_);
            map_base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        regs_ = nullptr;
    }

    void write32(size_t offset, uint32_t value) {
        if (dry_run_) return;
        regs_[offset / 4] = value;
        (void)regs_[offset / 4];
    }

    uint32_t read32(size_t offset) const {
        if (dry_run_) return 0;
        return regs_[offset / 4];
    }

private:
    bool dry_run_{false};
    int fd_{-1};
    size_t page_size_{0};
    void* map_base_{nullptr};
    size_t map_len_{0};
    volatile uint32_t* regs_{nullptr};
};

constexpr size_t MM2S_DMACR = 0x00;
constexpr size_t MM2S_DMASR = 0x04;
constexpr size_t MM2S_SA = 0x18;
constexpr size_t MM2S_LENGTH = 0x28;
constexpr size_t S2MM_DMACR = 0x30;
constexpr size_t S2MM_DMASR = 0x34;
constexpr size_t S2MM_DA = 0x48;
constexpr size_t S2MM_LENGTH = 0x58;

bool wait_for_bit(MmioRegion& dma, size_t offset, uint32_t mask,
                  std::chrono::milliseconds timeout, const char* label) {
    using namespace std::chrono_literals;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        uint32_t v = dma.read32(offset);
        if ((v & mask) == mask) {
            std::cout << "[dma] " << label << " status=0x" << std::hex << v << std::dec << "\n";
            return true;
        }
        if (v & 0x70) {
            std::cerr << "[dma] " << label << " error status=0x" << std::hex << v << std::dec << "\n";
            return false;
        }
        std::this_thread::sleep_for(100us);
    }
    std::cerr << "[dma] Timeout waiting for " << label << "\n";
    return false;
}

bool run_fft_dma_test(const Options& opts) {
    if (opts.dry_run) {
        std::cout << "[dma] Dry-run enabled; skipping DMA test\n";
        return true;
    }

    UdmaBuffer mm2s;
    UdmaBuffer s2mm;
    if (!mm2s.open(opts.mm2s_buf, opts.dry_run)) return false;
    if (!s2mm.open(opts.s2mm_buf, opts.dry_run)) {
        mm2s.close(opts.dry_run);
        return false;
    }

    size_t bytes = opts.samples * sizeof(uint32_t);
    if (bytes > mm2s.size || bytes > s2mm.size) {
        std::cerr << "[dma] Sample count exceeds u-dma-buf size\n";
        mm2s.close(opts.dry_run);
        s2mm.close(opts.dry_run);
        return false;
    }

    auto* tx = reinterpret_cast<uint32_t*>(mm2s.virt);
    auto* rx = reinterpret_cast<uint32_t*>(s2mm.virt);
    for (size_t i = 0; i < opts.samples; ++i) tx[i] = static_cast<uint32_t>(i);
    std::fill(rx, rx + opts.samples, 0);

    MmioRegion dma;
    if (!dma.open(opts.dma_base, opts.dma_span, opts.dry_run)) {
        mm2s.close(opts.dry_run);
        s2mm.close(opts.dry_run);
        return false;
    }

    dma.write32(MM2S_DMACR, 0x4);
    dma.write32(S2MM_DMACR, 0x4);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    dma.write32(MM2S_DMASR, 0xFFFFFFFF);
    dma.write32(S2MM_DMASR, 0xFFFFFFFF);

    dma.write32(S2MM_DMACR, 0x1);
    dma.write32(S2MM_DA, static_cast<uint32_t>(s2mm.phys));
    dma.write32(S2MM_LENGTH, static_cast<uint32_t>(bytes));

    dma.write32(MM2S_DMACR, 0x1);
    dma.write32(MM2S_SA, static_cast<uint32_t>(mm2s.phys));
    dma.write32(MM2S_LENGTH, static_cast<uint32_t>(bytes));

    bool ok_tx = wait_for_bit(dma, MM2S_DMASR, 1u << 12, opts.timeout, "MM2S");
    bool ok_rx = wait_for_bit(dma, S2MM_DMASR, 1u << 12, opts.timeout, "S2MM");

    auto tx_status = dma.read32(MM2S_DMASR);
    auto rx_status = dma.read32(S2MM_DMASR);
    std::cout << "[dma] Final status MM2S=0x" << std::hex << tx_status
              << " S2MM=0x" << rx_status << std::dec << "\n";

    if (ok_tx && ok_rx) {
        std::cout << "[dma] Transfer complete. Output samples:";
        size_t to_print = std::min<size_t>(8, opts.samples);
        for (size_t i = 0; i < to_print; ++i) std::cout << " " << rx[i];
        std::cout << "\n";
    }

    dma.close();
    mm2s.close(opts.dry_run);
    s2mm.close(opts.dry_run);
    return ok_tx && ok_rx;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_options(argc, argv, opts)) {
        print_usage();
        return 1;
    }
    if (opts.show_help) {
        print_usage();
        return 0;
    }
    if (opts.static_bit.empty()) {
        std::cerr << "Static bitstream path is required (use --static=...)\n";
        return 1;
    }

    namespace fs = std::filesystem;
    fs::path firmware_node(opts.manager_node);
    fs::path manager_dir = firmware_node.parent_path();
    fs::path flags_node = manager_dir / "flags";
    fs::path state_node = manager_dir / "state";

    fpga::FpgaManagerClient manager(opts.manager_node,
                                    flags_node.string(),
                                    state_node.string(),
                                    opts.firmware_dir,
                                    opts.dry_run);

    std::cout << "[fpga_fft_dma_loader] Loading static shell: " << opts.static_bit << "\n";
    if (!manager.load_bitstream(opts.static_bit, /*partial=*/false, opts.timeout)) {
        return 1;
    }

    if (opts.partial_bit.empty()) {
        std::cerr << "[fpga_fft_dma_loader] --partial is required for DMA validation\n";
        return 1;
    }

    fpga::DecoupleController decoupler(opts.dry_run);
    if (!decoupler.open(opts.gpio_base, opts.gpio_span)) {
        std::cerr << "[fpga_fft_dma_loader] Failed to map AXI GPIO\n";
        return 1;
    }

    std::cout << "[fpga_fft_dma_loader] Asserting DFX decouple\n";
    decoupler.set(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::cout << "[fpga_fft_dma_loader] Loading partial: " << opts.partial_bit << "\n";
    bool partial_ok = manager.load_bitstream(opts.partial_bit, /*partial=*/true, opts.timeout);

    std::cout << "[fpga_fft_dma_loader] Releasing DFX decouple\n";
    decoupler.set(false);

    if (!partial_ok) return 1;

    if (!run_fft_dma_test(opts)) return 1;

    std::cout << "[fpga_fft_dma_loader] DMA test complete\n";
    return 0;
}
