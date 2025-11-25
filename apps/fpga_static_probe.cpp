#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

struct OverlaySpec {
    std::string name;
    std::string path;
};

struct MmioRegion {
    std::string name;
    std::uint64_t phys = 0;
    std::size_t   length = 0;
    std::vector<std::uint32_t> offsets;
};

struct Options {
    bool trace_all = false;
    bool fpga_real = false;
    bool run_loopback = false;

    std::string fpga_manager = "/sys/class/fpga_manager/fpga0/firmware";
    std::string firmware_dir = "/lib/firmware";
    std::string static_bitstream;
    std::vector<OverlaySpec> overlays;

    std::vector<MmioRegion> mmio_regions;

    std::string udmabuf_name;
    std::string dma_device;
    std::string dma_region_name = "dma";
    std::size_t dma_map_size = 0;
    std::size_t loopback_bytes = 0;
    unsigned loopback_timeout_ms = 2000;

    std::optional<unsigned> pr_gpio;
    std::chrono::milliseconds pr_delay{0};
};

std::optional<std::uint64_t> parse_u64(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t idx = 0;
    try {
        auto value = std::stoull(text, &idx, 0);
        if (idx != text.size()) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::string hex(std::uint64_t value, unsigned width = 8) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
    return oss.str();
}

class GpioLine {
public:
    bool setup(unsigned line, std::chrono::milliseconds delay, bool mock) {
        line_ = line;
        delay_ = delay;
        mock_ = mock;
        base_path_ = "/sys/class/gpio/gpio" + std::to_string(line_);
        if (mock_) return true;
        if (!export_line()) return false;
        if (!write_file(base_path_ + "/direction", "out")) return false;
        value_fd_ = ::open((base_path_ + "/value").c_str(), O_WRONLY);
        if (value_fd_ < 0) {
            std::cerr << "gpio " << line_ << ": failed to open value: " << std::strerror(errno) << "\n";
            return false;
        }
        return true;
    }

    ~GpioLine() {
        if (value_fd_ >= 0) ::close(value_fd_);
    }

    bool freeze() { return set_value(1); }
    bool release() { return set_value(0); }

private:
    bool export_line() {
        if (std::filesystem::exists(base_path_)) return true;
        std::ofstream ofs("/sys/class/gpio/export");
        if (!ofs) {
            std::cerr << "gpio export failed: " << std::strerror(errno) << "\n";
            return false;
        }
        ofs << line_;
        ofs.close();
        for (int i = 0; i < 40; ++i) {
            if (std::filesystem::exists(base_path_)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::cerr << "gpio " << line_ << ": did not appear under sysfs" << "\n";
        return false;
    }

    bool write_file(const std::string& path, std::string_view value) {
        std::ofstream ofs(path);
        if (!ofs) {
            std::cerr << "gpio write " << path << " failed: " << std::strerror(errno) << "\n";
            return false;
        }
        ofs << value;
        return true;
    }

    bool set_value(int v) {
        if (mock_) return true;
        if (value_fd_ < 0) return false;
        if (::lseek(value_fd_, 0, SEEK_SET) < 0) {
            std::cerr << "gpio seek failed: " << std::strerror(errno) << "\n";
            return false;
        }
        char ch = v ? '1' : '0';
        if (::write(value_fd_, &ch, 1) != 1) {
            std::cerr << "gpio write failed: " << std::strerror(errno) << "\n";
            return false;
        }
        if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
        return true;
    }

    unsigned line_ = 0;
    bool mock_ = true;
    std::chrono::milliseconds delay_{0};
    std::string base_path_;
    int value_fd_ = -1;
};

class BitstreamManager {
public:
    BitstreamManager(std::string manager_path, std::string firmware_dir,
                     bool mock_mode, bool trace, GpioLine* decoupler)
        : manager_path_(std::move(manager_path)),
          firmware_dir_(std::move(firmware_dir)),
          mock_mode_(mock_mode),
          trace_(trace),
          decoupler_(decoupler) {}

    bool load_static(const std::string& path) {
        if (path.empty()) return true;
        if (trace_) std::cout << "[static] staging " << path << "\n";
        auto staged = stage_file("static", path);
        if (staged.empty()) return false;
        return write_manager(staged);
    }

    bool load_overlay(const std::string& name, const std::string& path) {
        if (trace_) std::cout << "[overlay] staging " << name << " from " << path << "\n";
        auto staged = stage_file(name, path);
        if (staged.empty()) return false;
        struct Guard {
            GpioLine* gpio;
            explicit Guard(GpioLine* g) : gpio(g) {}
            ~Guard() { if (gpio) gpio->release(); }
        } guard(decoupler_);
        if (decoupler_) {
            if (!decoupler_->freeze()) return false;
        }
        return write_manager(staged);
    }

private:
    static std::string sanitize(std::string_view label) {
        std::string out;
        out.reserve(label.size());
        for (char c : label) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        return out;
    }

    std::string stage_file(const std::string& label, const std::string& path) {
        if (mock_mode_) return label + "_mock";
        namespace fs = std::filesystem;
        fs::path src(path);
        if (!fs::exists(src)) {
            std::cerr << "bitstream " << path << " not found" << "\n";
            return {};
        }

        std::error_code ec;
        auto firmware_root = fs::path(firmware_dir_).lexically_normal();
        auto src_abs = fs::absolute(src).lexically_normal();

        auto firmware_str = firmware_root.string();
        if (!firmware_str.empty() && firmware_str.back() == '/') firmware_str.pop_back();
        auto src_str = src_abs.string();
        if (!src_str.empty() && src_str.back() == '/') src_str.pop_back();

        if (!firmware_str.empty() && src_str.rfind(firmware_str, 0) == 0) {
            if (trace_) std::cout << " source already under firmware dir\n";
            return src_abs.filename().string();
        }

        fs::create_directories(firmware_dir_, ec);
        if (ec) {
            std::cerr << "failed to create firmware dir " << firmware_dir_ << ": " << ec.message() << "\n";
            return {};
        }
        auto name = sanitize(label) + "_" + src.filename().string();
        fs::path dest = firmware_root / name;
        fs::copy_file(src_abs, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "copy to " << dest << " failed: " << ec.message() << "\n";
            return {};
        }
        if (trace_) std::cout << " staged as " << dest << "\n";
        return dest.filename().string();
    }

    bool write_manager(const std::string& firmware_name) {
        if (mock_mode_) {
            std::cout << "[mock] would load " << firmware_name << "\n";
            return true;
        }
        std::ofstream ofs(manager_path_);
        if (!ofs) {
            std::cerr << "open " << manager_path_ << " failed: " << std::strerror(errno) << "\n";
            return false;
        }
        ofs << firmware_name << std::endl;
        if (!ofs.good()) {
            std::cerr << "write to fpga manager failed" << "\n";
            return false;
        }
        if (trace_) std::cout << " requested reconfig: " << firmware_name << "\n";
        return true;
    }

    std::string manager_path_;
    std::string firmware_dir_;
    bool mock_mode_ = true;
    bool trace_ = false;
    GpioLine* decoupler_ = nullptr;
};

class MmioDumper {
public:
    bool dump(const MmioRegion& region) {
        if (region.offsets.empty()) return true;
        int fd = ::open("/dev/mem", O_RDONLY | O_SYNC);
        if (fd < 0) {
            std::cerr << "open /dev/mem failed: " << std::strerror(errno) << "\n";
            return false;
        }
        long page = sysconf(_SC_PAGESIZE);
        std::uint64_t page_base = region.phys & ~static_cast<std::uint64_t>(page - 1);
        std::size_t page_offset = static_cast<std::size_t>(region.phys - page_base);
        std::size_t map_len = page_offset + region.length;
        void* map = ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, static_cast<off_t>(page_base));
        if (map == MAP_FAILED) {
            std::cerr << "mmap failed: " << std::strerror(errno) << "\n";
            ::close(fd);
            return false;
        }
        auto* base = static_cast<std::uint8_t*>(map) + page_offset;
        std::cout << "[mmio] " << region.name << " base=" << hex(region.phys, 8) << "\n";
        for (auto offset : region.offsets) {
            if (offset + sizeof(std::uint32_t) > region.length) {
                std::cerr << " offset " << hex(offset) << " out of range" << "\n";
                continue;
            }
            auto* ptr = reinterpret_cast<volatile std::uint32_t*>(base + offset);
            auto value = *ptr;
            std::cout << "  +" << hex(offset, 4) << " = " << hex(value, 8) << "\n";
        }
        ::munmap(map, map_len);
        ::close(fd);
        return true;
    }
};

class UdmaBuffer {
public:
    UdmaBuffer() = default;
    ~UdmaBuffer() { close(); }

    bool open(const std::string& dev_name) {
        close();
        std::string device_path = dev_name;
        if (device_path.find('/') == std::string::npos) device_path = "/dev/" + device_path;
        std::string sysfs_name = dev_name;
        if (sysfs_name.find('/') != std::string::npos) sysfs_name = std::filesystem::path(sysfs_name).filename().string();
        sysfs_base_ = std::string("/sys/class/udmabuf/") + sysfs_name;
        if (!read_sysfs("phys_addr", phys_) || !read_size()) return false;
        fd_ = ::open(device_path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "open " << device_path << " failed: " << std::strerror(errno) << "\n";
            return false;
        }
        map_ = static_cast<std::uint8_t*>(::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (map_ == MAP_FAILED) {
            std::cerr << "mmap udmabuf failed: " << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            map_ = nullptr;
            return false;
        }
        return true;
    }

    void close() {
        if (map_ && map_ != MAP_FAILED) {
            ::munmap(map_, size_);
            map_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::uint8_t* data() const { return map_; }
    std::size_t size() const { return size_; }
    std::uint64_t phys() const { return phys_; }

    bool sync(std::size_t offset, std::size_t len, int flags) const {
        if (!map_) return false;
        if (::msync(map_ + offset, len, flags) != 0) {
            std::cerr << "msync failed: " << std::strerror(errno) << "\n";
            return false;
        }
        return true;
    }

private:
    bool read_size() {
        std::ifstream ifs(sysfs_base_ + "/size");
        if (!ifs) {
            std::cerr << "udmabuf size not readable at " << sysfs_base_ << "\n";
            return false;
        }
        ifs >> size_;
        return true;
    }

    bool read_sysfs(const std::string& field, std::uint64_t& value) {
        std::ifstream ifs(sysfs_base_ + "/" + field);
        if (!ifs) {
            std::cerr << "udmabuf sysfs missing: " << sysfs_base_ << "\n";
            return false;
        }
        ifs >> std::hex >> value;
        return true;
    }

    std::string sysfs_base_;
    int fd_ = -1;
    std::uint8_t* map_ = nullptr;
    std::size_t size_ = 0;
    std::uint64_t phys_ = 0;
};

class AxiDma {
public:
    ~AxiDma() { close(); }

    bool open(const std::string& device, std::size_t map_size) {
        close();
        fd_ = ::open(device.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "open " << device << " failed: " << std::strerror(errno) << "\n";
            return false;
        }
        regs_ = static_cast<std::uint8_t*>(::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (regs_ == MAP_FAILED) {
            std::cerr << "dma mmap failed: " << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            regs_ = nullptr;
            return false;
        }
        map_size_ = map_size;
        return true;
    }

    void close() {
        if (regs_ && regs_ != MAP_FAILED) {
            ::munmap(regs_, map_size_);
            regs_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool reset_channel(std::uint32_t ctrl_offset) {
        write_reg(ctrl_offset, 0x4);
        auto start = std::chrono::steady_clock::now();
        while (read_reg(ctrl_offset) & 0x4) {
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) {
                std::cerr << "dma reset timeout" << "\n";
                return false;
            }
        }
        return true;
    }

    bool wait_idle(std::uint32_t status_offset, std::chrono::milliseconds timeout, const char* label) {
        static constexpr std::uint32_t kErrMask =
            (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) |
            (1u << 10) | (1u << 11) | (1u << 14);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto status = read_reg(status_offset);
            if (status & kErrMask) {
                std::cerr << "dma " << label << " error status=" << hex(status) << "\n";
                return false;
            }
            if (status & (1u << 12)) {
                write_reg(status_offset, status & ((1u << 12) | (1u << 13) | (1u << 14)));
                return true;
            }
            if (status & (1u << 1)) return true;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::cerr << "dma " << label << " wait timeout" << "\n";
        return false;
    }

    bool loopback(std::uint64_t tx, std::uint64_t rx, std::size_t bytes, std::chrono::milliseconds timeout) {
        if (!regs_) return false;
        if (!reset_channel(kMm2sCtrl) || !reset_channel(kS2mmCtrl)) return false;
        write_reg(kMm2sStatus, 0xFFFFFFFF);
        write_reg(kS2mmStatus, 0xFFFFFFFF);
        write_reg(kS2mmCtrl, 1);
        write_reg(kMm2sCtrl, 1);
        write_reg(kMm2sSrc, static_cast<std::uint32_t>(tx));
        write_reg(kMm2sSrc + 4, static_cast<std::uint32_t>(tx >> 32));
        write_reg(kS2mmDst, static_cast<std::uint32_t>(rx));
        write_reg(kS2mmDst + 4, static_cast<std::uint32_t>(rx >> 32));
        write_reg(kS2mmLen, static_cast<std::uint32_t>(bytes));
        write_reg(kMm2sLen, static_cast<std::uint32_t>(bytes));
        if (!wait_idle(kS2mmStatus, timeout, "s2mm")) return false;
        if (!wait_idle(kMm2sStatus, timeout, "mm2s")) return false;
        return true;
    }

private:
    std::uint32_t read_reg(std::uint32_t offset) const {
        auto* ptr = reinterpret_cast<volatile std::uint32_t*>(regs_ + offset);
        return *ptr;
    }
    void write_reg(std::uint32_t offset, std::uint32_t value) {
        auto* ptr = reinterpret_cast<volatile std::uint32_t*>(regs_ + offset);
        *ptr = value;
    }

    int fd_ = -1;
    std::uint8_t* regs_ = nullptr;
    std::size_t map_size_ = 0;

    static constexpr std::uint32_t kMm2sCtrl = 0x00;
    static constexpr std::uint32_t kMm2sStatus = 0x04;
    static constexpr std::uint32_t kMm2sSrc = 0x18;
    static constexpr std::uint32_t kMm2sLen = 0x28;
    static constexpr std::uint32_t kS2mmCtrl = 0x30;
    static constexpr std::uint32_t kS2mmStatus = 0x34;
    static constexpr std::uint32_t kS2mmDst = 0x48;
    static constexpr std::uint32_t kS2mmLen = 0x58;
};

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --static-bitstream=PATH\n"
              << "  --overlay=name:PATH (repeatable)\n"
              << "  --fpga-manager=PATH\n"
              << "  --firmware-dir=DIR (default /lib/firmware)\n"
              << "  --fpga-real | --fpga-mock\n"
              << "  --fpga-pr-gpio=N [--fpga-pr-gpio-delay-ms=MS]\n"
              << "  --mmio-probe=name:addr:size [--mmio-probe-offset=name:offset]\n"
              << "  --run-loopback --udmabuf=NAME --dma-device=PATH --bytes=N\n"
              << "  [--dma-map-size=N] [--dma-mmio-region=name] [--loopback-timeout-ms=MS]\n"
              << "  --trace-all\n";
}

bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--trace-all") { opts.trace_all = true; continue; }
        if (arg == "--fpga-real") { opts.fpga_real = true; continue; }
        if (arg == "--fpga-mock") { opts.fpga_real = false; continue; }
        auto consume = [&](const std::string& prefix) -> std::optional<std::string> {
            if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
            return std::nullopt;
        };
        if (auto v = consume("--static-bitstream=")) { opts.static_bitstream = *v; continue; }
        if (auto v = consume("--overlay=")) {
            auto pos = v->find(':');
            if (pos == std::string::npos) {
                std::cerr << "overlay requires name:path" << "\n";
                return false;
            }
            opts.overlays.push_back({v->substr(0, pos), v->substr(pos + 1)});
            continue;
        }
        if (auto v = consume("--mmio-probe=")) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            while (start < v->size()) {
                auto pos = v->find(':', start);
                if (pos == std::string::npos) pos = v->size();
                parts.push_back(v->substr(start, pos - start));
                start = pos + 1;
            }
            if (parts.size() != 3) {
                std::cerr << "mmio probe format name:addr:size" << "\n";
                return false;
            }
            auto addr = parse_u64(parts[1]);
            auto len = parse_u64(parts[2]);
            if (!addr || !len) {
                std::cerr << "mmio probe parse failed" << "\n";
                return false;
            }
            if (*len == 0) {
                std::cerr << "mmio region length must be >0" << "\n";
                return false;
            }
            opts.mmio_regions.push_back({parts[0], *addr, static_cast<std::size_t>(*len), {0}});
            continue;
        }
        if (auto v = consume("--mmio-probe-offset=")) {
            auto pos = v->find(':');
            if (pos == std::string::npos) {
                std::cerr << "mmio offset requires name:offset" << "\n";
                return false;
            }
            auto name = v->substr(0, pos);
            auto offset = parse_u64(v->substr(pos + 1));
            if (!offset) {
                std::cerr << "offset parse failed" << "\n";
                return false;
            }
            auto it = std::find_if(opts.mmio_regions.begin(), opts.mmio_regions.end(),
                                   [&](const MmioRegion& r){ return r.name == name; });
            if (it == opts.mmio_regions.end()) {
                std::cerr << "mmio region " << name << " not defined" << "\n";
                return false;
            }
            it->offsets.push_back(static_cast<std::uint32_t>(*offset));
            continue;
        }
        if (arg == "--run-loopback") { opts.run_loopback = true; continue; }
        if (auto v = consume("--udmabuf=")) { opts.udmabuf_name = *v; continue; }
        if (auto v = consume("--dma-device=")) { opts.dma_device = *v; continue; }
        if (auto v = consume("--bytes=")) {
            auto val = parse_u64(*v);
            if (!val) { std::cerr << "invalid bytes" << "\n"; return false; }
            opts.loopback_bytes = static_cast<std::size_t>(*val);
            continue;
        }
        if (auto v = consume("--dma-map-size=")) {
            auto val = parse_u64(*v);
            if (!val) { std::cerr << "invalid dma map size" << "\n"; return false; }
            opts.dma_map_size = static_cast<std::size_t>(*val);
            continue;
        }
        if (auto v = consume("--dma-mmio-region=")) { opts.dma_region_name = *v; continue; }
        if (auto v = consume("--loopback-timeout-ms=")) {
            auto val = parse_u64(*v);
            if (!val) { std::cerr << "invalid timeout" << "\n"; return false; }
            opts.loopback_timeout_ms = static_cast<unsigned>(*val);
            continue;
        }
        if (auto v = consume("--fpga-manager=")) { opts.fpga_manager = *v; continue; }
        if (auto v = consume("--firmware-dir=")) { opts.firmware_dir = *v; continue; }
        if (auto v = consume("--fpga-pr-gpio=")) {
            auto val = parse_u64(*v);
            if (!val) { std::cerr << "invalid gpio" << "\n"; return false; }
            opts.pr_gpio = static_cast<unsigned>(*val);
            continue;
        }
        if (auto v = consume("--fpga-pr-gpio-delay-ms=")) {
            auto val = parse_u64(*v);
            if (!val) { std::cerr << "invalid delay" << "\n"; return false; }
            opts.pr_delay = std::chrono::milliseconds(*val);
            continue;
        }
        std::cerr << "Unknown argument: " << arg << "\n";
        return false;
    }
    return true;
}

const MmioRegion* find_region(const Options& opts, const std::string& name) {
    for (const auto& region : opts.mmio_regions) if (region.name == name) return &region;
    return nullptr;
}

bool run_loopback(const Options& opts) {
    if (opts.udmabuf_name.empty() || opts.dma_device.empty() || opts.loopback_bytes == 0) {
        std::cerr << "loopback requires --udmabuf, --dma-device, --bytes" << "\n";
        return false;
    }
    UdmaBuffer buf;
    if (!buf.open(opts.udmabuf_name)) return false;
    std::size_t bytes = opts.loopback_bytes;
    std::size_t total_needed = bytes * 2;
    if (buf.size() < total_needed) {
        std::cerr << "udmabuf size " << buf.size() << " insufficient for " << total_needed << " bytes" << "\n";
        return false;
    }
    auto* tx = buf.data();
    auto* rx = buf.data() + bytes;
    for (std::size_t i = 0; i < bytes; ++i) {
        tx[i] = static_cast<std::uint8_t>((i * 131) & 0xFF);
        rx[i] = 0;
    }
    buf.sync(0, total_needed, MS_SYNC);

    std::size_t dma_map = opts.dma_map_size;
    if (dma_map == 0) {
        if (auto* region = find_region(opts, opts.dma_region_name)) dma_map = region->length;
    }
    if (dma_map == 0) dma_map = 0x1000;

    AxiDma dma;
    if (!dma.open(opts.dma_device, dma_map)) return false;
    auto start = std::chrono::steady_clock::now();
    if (!dma.loopback(buf.phys(), buf.phys() + bytes, bytes,
                      std::chrono::milliseconds(opts.loopback_timeout_ms))) {
        return false;
    }
    auto stop = std::chrono::steady_clock::now();
    buf.sync(bytes, bytes, MS_INVALIDATE);

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (tx[i] != rx[i]) {
            if (mismatches < 8) {
                std::cerr << "mismatch at " << i << ": tx=" << static_cast<int>(tx[i])
                          << " rx=" << static_cast<int>(rx[i]) << "\n";
            }
            ++mismatches;
        }
    }
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    double seconds = duration.count() / 1'000'000.0;
    double mb = bytes / (1024.0 * 1024.0);
    double mbps = seconds > 0 ? mb / seconds : 0.0;
    std::cout << "[loopback] " << bytes << " bytes in " << duration.count() / 1000.0 << " ms ("
              << std::fixed << std::setprecision(2) << mbps << " MB/s)" << "\n";
    if (mismatches == 0) {
        std::cout << "[loopback] data verified OK" << "\n";
        return true;
    }
    std::cerr << "[loopback] mismatches: " << mismatches << "\n";
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return 1;
    }

    GpioLine pr_gpio;
    if (opts.pr_gpio) {
        if (!pr_gpio.setup(*opts.pr_gpio, opts.pr_delay, !opts.fpga_real)) {
            return 1;
        }
    }

    BitstreamManager manager(opts.fpga_manager, opts.firmware_dir, !opts.fpga_real, opts.trace_all,
                             opts.pr_gpio ? &pr_gpio : nullptr);

    bool ok = true;
    if (!opts.static_bitstream.empty()) {
        ok &= manager.load_static(opts.static_bitstream);
    }
    for (const auto& overlay : opts.overlays) {
        ok &= manager.load_overlay(overlay.name, overlay.path);
    }

    MmioDumper dumper;
    for (const auto& region : opts.mmio_regions) {
        ok &= dumper.dump(region);
    }

    if (opts.run_loopback) {
        ok &= run_loopback(opts);
    }

    return ok ? 0 : 1;
}
