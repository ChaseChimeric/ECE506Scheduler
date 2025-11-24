#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>

namespace {
struct Options {
    std::string device = "/dev/axi_dma_regs";
    std::string udmabuf = "udmabuf0";
    size_t bytes = 256 * 1024; // 256 KiB
    unsigned timeout_ms = 100;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [--device=/dev/axi_dma_regs]"
              << " [--udmabuf=udmabuf0] [--bytes=N] [--timeout-ms=N]\n";
}

bool parse_size(const std::string& text, size_t* out) {
    try {
        *out = static_cast<size_t>(std::stoull(text, nullptr, 0));
        return true;
    } catch (...) {
        return false;
    }
}

bool read_uint64(const std::string& path, uint64_t* out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    std::string text;
    ifs >> text;
    try {
        *out = std::stoull(text, nullptr, 0);
    } catch (...) {
        return false;
    }
    return true;
}

struct UdmabufRegion {
    bool init(const std::string& name) {
        std::string base = "/sys/class/u-dma-buf/" + name;
        uint64_t size_value = 0;
        if (!read_uint64(base + "/size", &size_value)) {
            std::cerr << "[udmabuf] failed to read size for " << name << "\n";
            return false;
        }
        if (!read_uint64(base + "/phys_addr", &phys_)) {
            std::cerr << "[udmabuf] failed to read phys addr for " << name << "\n";
            return false;
        }
        std::string dev_path = "/dev/" + name;
        fd_ = ::open(dev_path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::perror(("[udmabuf] open " + dev_path).c_str());
            return false;
        }
        void* map = mmap(nullptr, size_value, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (map == MAP_FAILED) {
            std::perror("[udmabuf] mmap");
            close(fd_);
            fd_ = -1;
            return false;
        }
        virt_ = static_cast<uint8_t*>(map);
        size_ = static_cast<size_t>(size_value);
        return true;
    }

    ~UdmabufRegion() {
        if (virt_) munmap(virt_, size_);
        if (fd_ >= 0) close(fd_);
    }

    uint8_t* virt() const { return virt_; }
    size_t size() const { return size_; }
    uint64_t phys() const { return phys_; }

private:
    int fd_{-1};
    uint8_t* virt_{nullptr};
    size_t size_{0};
    uint64_t phys_{0};
};

struct Device {
    explicit Device(std::string path) : path_(std::move(path)) {}
    bool open_rw() {
        fd_ = ::open(path_.c_str(), O_RDWR);
        if (fd_ < 0) {
            std::perror(("[axi-dma-test] open " + path_).c_str());
            return false;
        }
        return true;
    }
    ~Device() {
        if (fd_ >= 0) close(fd_);
    }

    uint32_t read(off_t offset) const {
        uint32_t value = 0;
        if (pread(fd_, &value, sizeof(value), offset) != sizeof(value)) {
            std::perror("[axi-dma-test] pread");
        }
        return value;
    }

    void write(off_t offset, uint32_t value) const {
        if (pwrite(fd_, &value, sizeof(value), offset) != sizeof(value)) {
            std::perror("[axi-dma-test] pwrite");
        }
    }

    int fd() const { return fd_; }

private:
    std::string path_;
    int fd_{-1};
};

constexpr off_t MM2S_DMACR = 0x00;
constexpr off_t MM2S_DMASR = 0x04;
constexpr off_t MM2S_SA = 0x18;
constexpr off_t MM2S_SA_MSB = 0x1C;
constexpr off_t MM2S_LENGTH = 0x28;
constexpr off_t S2MM_DMACR = 0x30;
constexpr off_t S2MM_DMASR = 0x34;
constexpr off_t S2MM_DA = 0x48;
constexpr off_t S2MM_DA_MSB = 0x4C;
constexpr off_t S2MM_LENGTH = 0x58;
constexpr uint32_t DMA_CR_RUNSTOP = 0x1;
constexpr uint32_t DMA_CR_IOC_IrqEn = 0x10;
constexpr uint32_t DMA_CR_ERR_IrqEn = 0x40;
constexpr uint32_t DMA_SR_IDLE = 0x2;
constexpr uint32_t DMA_SR_ERR_MASK = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                     (1u << 12) | (1u << 13) | (1u << 14);

bool wait_for_idle(const Device& dev, off_t status_reg, unsigned timeout_ms, const char* tag) {
    const unsigned polls = timeout_ms * 4; // poll every 250us
    for (unsigned i = 0; i < polls; ++i) {
        auto status = dev.read(status_reg);
        if (status & DMA_SR_ERR_MASK) {
            std::cerr << "[axi-dma-test] " << tag << " error status=0x" << std::hex << status << std::dec << "\n";
            return false;
        }
        if (status & DMA_SR_IDLE) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    std::cerr << "[axi-dma-test] " << tag << " timeout status=0x" << std::hex << dev.read(status_reg)
              << std::dec << "\n";
    return false;
}

int run_test(const Options& opts) {
    UdmabufRegion buf;
    if (!buf.init(opts.udmabuf)) return 1;
    size_t half = buf.size() / 2;
    if (half == 0) {
        std::cerr << "[axi-dma-test] udmabuf too small\n";
        return 1;
    }
    size_t bytes = opts.bytes ? opts.bytes : half;
    if (bytes > half) {
        std::cerr << "[axi-dma-test] requested bytes exceed half buffer (" << half << ")\n";
        return 1;
    }
    uint8_t* in = buf.virt();
    uint8_t* out = buf.virt() + half;
    for (size_t i = 0; i < bytes; ++i) in[i] = static_cast<uint8_t>(i & 0xFF);
    std::memset(out, 0, bytes);

    Device dev(opts.device);
    if (!dev.open_rw()) return 1;

    auto clear_status = [&](bool s2mm) {
        dev.write(s2mm ? S2MM_DMASR : MM2S_DMASR, 0xFFFFFFFF);
    };
    clear_status(true);
    clear_status(false);

    dev.write(S2MM_DMACR, DMA_CR_RUNSTOP | DMA_CR_IOC_IrqEn | DMA_CR_ERR_IrqEn);
    dev.write(S2MM_DA, static_cast<uint32_t>(buf.phys() + half));
    dev.write(S2MM_DA_MSB, static_cast<uint32_t>((buf.phys() + half) >> 32));
    dev.write(S2MM_LENGTH, static_cast<uint32_t>(bytes));

    dev.write(MM2S_DMACR, DMA_CR_RUNSTOP | DMA_CR_IOC_IrqEn | DMA_CR_ERR_IrqEn);
    dev.write(MM2S_SA, static_cast<uint32_t>(buf.phys()));
    dev.write(MM2S_SA_MSB, static_cast<uint32_t>(buf.phys() >> 32));
    dev.write(MM2S_LENGTH, static_cast<uint32_t>(bytes));

    bool mm2s_ok = wait_for_idle(dev, MM2S_DMASR, opts.timeout_ms, "mm2s");
    bool s2mm_ok = wait_for_idle(dev, S2MM_DMASR, opts.timeout_ms, "s2mm");
    uint32_t mm2s_sr = dev.read(MM2S_DMASR);
    uint32_t s2mm_sr = dev.read(S2MM_DMASR);
    std::cout << "[axi-dma-test] mm2s_sr=0x" << std::hex << mm2s_sr
              << " s2mm_sr=0x" << s2mm_sr << std::dec << "\n";

    if (!mm2s_ok || !s2mm_ok) {
        std::cerr << "[axi-dma-test] transfer did not complete.\n";
        return 1;
    }

    size_t mismatches = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (in[i] != out[i]) {
            if (mismatches < 8) {
                std::cerr << "[axi-dma-test] mismatch @+" << i
                          << " in=0x" << std::hex << static_cast<int>(in[i])
                          << " out=0x" << static_cast<int>(out[i]) << std::dec << "\n";
            }
            ++mismatches;
        }
    }
    if (mismatches == 0) {
        std::cout << "[axi-dma-test] SUCCESS: output matches input (" << bytes << " bytes)\n";
        return 0;
    }
    std::cerr << "[axi-dma-test] output mismatches: " << mismatches << "\n";
    return 1;
}
} // namespace

int main(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg.rfind("--device=", 0) == 0) {
            opts.device = arg.substr(sizeof("--device=") - 1);
            continue;
        }
        if (arg.rfind("--udmabuf=", 0) == 0) {
            opts.udmabuf = arg.substr(sizeof("--udmabuf=") - 1);
            continue;
        }
        if (arg.rfind("--bytes=", 0) == 0) {
            if (!parse_size(arg.substr(sizeof("--bytes=") - 1), &opts.bytes)) {
                std::cerr << "Invalid bytes value\n";
                return 1;
            }
            continue;
        }
        if (arg.rfind("--timeout-ms=", 0) == 0) {
            size_t tmp = 0;
            if (!parse_size(arg.substr(sizeof("--timeout-ms=") - 1), &tmp)) {
                std::cerr << "Invalid timeout\n";
                return 1;
            }
            opts.timeout_ms = static_cast<unsigned>(tmp);
            continue;
        }
        std::cerr << "Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return run_test(opts);
}
