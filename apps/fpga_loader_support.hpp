#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace fpga {

class DecoupleController {
public:
    explicit DecoupleController(bool dry_run) : dry_run_(dry_run) {}
    ~DecoupleController() { close(); }

    bool open(uint64_t phys_addr, size_t span) {
        if (opened_) return true;
        if (dry_run_) {
            opened_ = true;
            return true;
        }
        page_size_ = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        if (page_size_ == 0) page_size_ = 4096;
        off_t page_base = static_cast<off_t>(phys_addr & ~static_cast<uint64_t>(page_size_ - 1));
        size_t page_offset = static_cast<size_t>(phys_addr - static_cast<uint64_t>(page_base));
        map_len_ = ((page_offset + span + page_size_ - 1) / page_size_) * page_size_;
        fd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd_ < 0) {
            std::cerr << "[fpga_loader] Failed to open /dev/mem: " << std::strerror(errno) << "\n";
            return false;
        }
        map_base_ = ::mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, page_base);
        if (map_base_ == MAP_FAILED) {
            std::cerr << "[fpga_loader] mmap failed: " << std::strerror(errno) << "\n";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        regs_ = reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(map_base_) + page_offset);
        regs_[1] = 0x0; // channel outputs enabled
        opened_ = true;
        return true;
    }

    bool set(bool asserted) {
        if (!opened_) return false;
        if (dry_run_) return true;
        regs_[0] = asserted ? 0x1 : 0x0;
        (void)regs_[0];
        current_value_ = asserted;
        return true;
    }

    void close() {
        if (dry_run_) {
            opened_ = false;
            return;
        }
        if (map_base_ && map_base_ != MAP_FAILED) {
            ::munmap(map_base_, map_len_);
            map_base_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        regs_ = nullptr;
        opened_ = false;
    }

private:
    bool dry_run_;
    bool opened_{false};
    bool current_value_{false};
    int fd_{-1};
    size_t page_size_{0};
    void* map_base_{nullptr};
    size_t map_len_{0};
    volatile uint32_t* regs_{nullptr};
};

class FpgaManagerClient {
public:
    FpgaManagerClient(std::string firmware_node,
                      std::string flags_node,
                      std::string state_node,
                      std::string staging_dir,
                      bool dry_run)
        : firmware_node_(std::move(firmware_node)),
          flags_node_(std::move(flags_node)),
          state_node_(std::move(state_node)),
          staging_dir_(std::move(staging_dir)),
          dry_run_(dry_run) {}

    bool load_bitstream(const std::string& source, bool partial,
                        std::chrono::milliseconds timeout) const {
        std::string firmware_name;
        if (!stage_file(source, firmware_name)) return false;
        bool flags_set = false;
        if (partial && !flags_node_.empty()) {
            if (!set_flags(1)) return false;
            flags_set = true;
        }
        bool ok = request_firmware(firmware_name);
        if (flags_set) {
            if (!set_flags(0)) return false;
        }
        return ok && wait_for_completion(timeout);
    }

private:
    bool stage_file(const std::string& source, std::string& firmware_name) const {
        namespace fs = std::filesystem;
        fs::path src(source);
        if (!fs::exists(src)) {
            std::cerr << "[fpga_loader] Missing bitstream: " << source << "\n";
            return false;
        }
        firmware_name = src.filename().string();
        if (dry_run_) return true;

        fs::path target_dir = staging_dir_.empty() ? src.parent_path() : fs::path(staging_dir_);
        if (target_dir.empty()) target_dir = fs::current_path();
        fs::path dest = target_dir / firmware_name;

        std::error_code ec;
        if (fs::equivalent(src, dest, ec)) {
            return true;
        }
        ec.clear();
        fs::create_directories(target_dir, ec);
        if (ec) {
            std::cerr << "[fpga_loader] Failed to create firmware dir '" << target_dir
                      << "': " << ec.message() << "\n";
            return false;
        }
        ec.clear();
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "[fpga_loader] Failed to copy " << src << " -> " << dest
                      << ": " << ec.message() << "\n";
            return false;
        }
        std::cout << "[fpga_loader] Staged " << src << " -> " << dest << "\n";
        return true;
    }

    bool set_flags(uint32_t value) const {
        if (dry_run_ || flags_node_.empty()) return true;
        std::ofstream ofs(flags_node_);
        if (!ofs) {
            std::cerr << "[fpga_loader] Unable to open " << flags_node_ << ": "
                      << std::strerror(errno) << "\n";
            return false;
        }
        ofs << value << std::endl;
        if (!ofs.good()) {
            std::cerr << "[fpga_loader] Failed to write flags " << value << "\n";
            return false;
        }
        return true;
    }

    bool request_firmware(const std::string& firmware_name) const {
        std::cout << "[fpga_loader] Programming " << firmware_name << "\n";
        if (dry_run_) return true;
        std::ofstream ofs(firmware_node_);
        if (!ofs) {
            std::cerr << "[fpga_loader] Unable to open " << firmware_node_ << ": "
                      << std::strerror(errno) << "\n";
            return false;
        }
        ofs << firmware_name << std::endl;
        if (!ofs.good()) {
            std::cerr << "[fpga_loader] Failed to request firmware " << firmware_name << "\n";
            return false;
        }
        return true;
    }

    std::string read_state() const {
        if (dry_run_ || state_node_.empty()) return "dry-run";
        std::ifstream ifs(state_node_);
        if (!ifs) return {};
        std::string line;
        std::getline(ifs, line);
        return line;
    }

    bool wait_for_completion(std::chrono::milliseconds timeout) const {
        if (dry_run_ || state_node_.empty()) return true;
        auto start = std::chrono::steady_clock::now();
        std::string last_state;
        while (std::chrono::steady_clock::now() - start < timeout) {
            std::string state = read_state();
            if (state.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (state != last_state) {
                std::cout << "[fpga_loader] fpga_manager state -> " << state << "\n";
                last_state = state;
            }
            if (state.find("error") != std::string::npos) {
                std::cerr << "[fpga_loader] fpga_manager reported error\n";
                return false;
            }
            if (state.find("operating") != std::string::npos ||
                state.find("unknown") != std::string::npos ||
                state.find("user") != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "[fpga_loader] Timeout waiting for fpga_manager completion\n";
        return false;
    }

    std::string firmware_node_;
    std::string flags_node_;
    std::string state_node_;
    std::string staging_dir_;
    bool dry_run_;
};

} // namespace fpga
