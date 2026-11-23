#include "sdrsource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace hritapp {
namespace {

constexpr std::size_t kRingFrames = 1u << 22;   // 4 Mi frames, 32 MiB of complex float

#ifdef _WIN32
using LibHandle = HMODULE;
#else
using LibHandle = void*;
#endif

std::wstring widen(const std::string& s) { return std::filesystem::path(s).wstring(); }

std::wstring executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return {};
    return std::filesystem::path(std::wstring(buf.data(), n)).parent_path().wstring();
#else
    return {};
#endif
}

class DynamicLibrary {
public:
    ~DynamicLibrary() { close(); }
    DynamicLibrary() = default;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    bool open(const std::vector<std::string>& names, std::string& error) {
        close();
#ifdef _WIN32
        // A missing entry point in a dependency must become a normal error, not a modal Windows
        // dialog that blocks the application.
        static std::once_flag once;
        std::call_once(once, [] { SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX); });
        const std::wstring dir = executableDirectory();
        DWORD lastError = 0;
        for (const std::string& name : names) {
            std::vector<std::wstring> candidates;
            if (!dir.empty()) {
                // A dedicated sdr\ subfolder keeps the matching libusb/pthread DLLs together and
                // avoids mixing versions from different SDR packages.
                candidates.push_back((std::filesystem::path(dir) / L"sdr" / widen(name)).wstring());
                candidates.push_back((std::filesystem::path(dir) / widen(name)).wstring());
            }
            for (const std::wstring& full : candidates) {
                if (!std::filesystem::exists(full)) continue;
                handle_ = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (handle_) return true;
                lastError = GetLastError();
            }
            handle_ = LoadLibraryW(widen(name).c_str());
            if (handle_) return true;
            lastError = GetLastError();
        }
        error = "无法加载 " + names.front() + "（Windows 错误 " + std::to_string(lastError) + "）";
        if (lastError == ERROR_PROC_NOT_FOUND || lastError == ERROR_MOD_NOT_FOUND ||
            lastError == ERROR_DLL_NOT_FOUND) {
            error += "：依赖 DLL 版本不匹配。请把同一套 SDR 库（含 libusb-1.0.dll、pthreadVC3.dll）"
                     "放到程序目录或 sdr\\ 子目录，不要混用不同来源的版本。";
        } else {
            error += "；请把库文件放到程序目录或 sdr\\ 子目录。";
        }
        return false;
#else
        for (const std::string& name : names) {
            handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (handle_) return true;
        }
        error = "无法加载 " + names.front() + "；请把库文件放到程序目录，或加入系统 PATH。";
        return false;
#endif
    }

    void close() {
        if (!handle_) return;
#ifdef _WIN32
        FreeLibrary(handle_);
#else
        dlclose(handle_);
#endif
        handle_ = nullptr;
    }

    void* symbol(const char* name) const {
        if (!handle_) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle_, name));
#else
        return dlsym(handle_, name);
#endif
    }

private:
    LibHandle handle_ = nullptr;
};

std::vector<std::string> hackrfLibraryNames() {
#ifdef _WIN32
    return {"hackrf.dll", "libhackrf.dll"};
#else
    return {"libhackrf.so.0", "libhackrf.so"};
#endif
}

std::vector<std::string> rtlsdrLibraryNames() {
#ifdef _WIN32
    return {"rtlsdr.dll", "librtlsdr.dll"};
#else
    return {"librtlsdr.so.0", "librtlsdr.so"};
#endif
}

// ---- HackRF ABI (public C API; loaded at run time) ------------------------------------------------
struct hackrf_device;
struct hackrf_transfer {
    hackrf_device* device;
    std::uint8_t* buffer;
    std::uint32_t buffer_length;
    std::int32_t valid_length;
    void* rx_ctx;
    void* tx_ctx;
};
struct hackrf_device_list {
    char** serial_numbers;
    int* usb_board_ids;
    int* usb_device_index;
    int devicecount;
    void** usb_devices;
    int usb_devicecount;
};
using hackrf_sample_block_cb_fn = int (*)(hackrf_transfer*);
using hackrf_init_fn = int (*)();
using hackrf_exit_fn = int (*)();
using hackrf_open_fn = int (*)(hackrf_device**);
using hackrf_open_by_serial_fn = int (*)(const char*, hackrf_device**);
using hackrf_close_fn = int (*)(hackrf_device*);
using hackrf_set_sample_rate_fn = int (*)(hackrf_device*, double);
using hackrf_set_filter_fn = int (*)(hackrf_device*, std::uint32_t);
using hackrf_compute_filter_fn = std::uint32_t (*)(std::uint32_t);
using hackrf_set_freq_fn = int (*)(hackrf_device*, std::uint64_t);
using hackrf_set_gain_fn = int (*)(hackrf_device*, std::uint32_t);
using hackrf_set_amp_fn = int (*)(hackrf_device*, std::uint8_t);
using hackrf_set_antenna_fn = int (*)(hackrf_device*, std::uint8_t);
using hackrf_start_rx_fn = int (*)(hackrf_device*, hackrf_sample_block_cb_fn, void*);
using hackrf_stop_rx_fn = int (*)(hackrf_device*);
using hackrf_is_streaming_fn = int (*)(hackrf_device*);
using hackrf_device_list_fn = hackrf_device_list* (*)();
using hackrf_device_list_free_fn = void (*)(hackrf_device_list*);

// ---- RTL-SDR ABI (public C API; loaded at run time) -----------------------------------------------
struct rtlsdr_dev;
using rtlsdr_read_async_cb_t = void (*)(unsigned char*, std::uint32_t, void*);
using rtlsdr_get_device_count_fn = std::uint32_t (*)();
using rtlsdr_get_device_name_fn = const char* (*)(std::uint32_t);
using rtlsdr_get_device_usb_strings_fn = int (*)(std::uint32_t, char*, char*, char*);
using rtlsdr_open_fn = int (*)(rtlsdr_dev**, std::uint32_t);
using rtlsdr_close_fn = int (*)(rtlsdr_dev*);
using rtlsdr_set_sample_rate_fn = int (*)(rtlsdr_dev*, std::uint32_t);
using rtlsdr_get_sample_rate_fn = std::uint32_t (*)(rtlsdr_dev*);
using rtlsdr_set_center_freq_fn = int (*)(rtlsdr_dev*, std::uint32_t);
using rtlsdr_set_freq_correction_fn = int (*)(rtlsdr_dev*, int);
using rtlsdr_set_agc_mode_fn = int (*)(rtlsdr_dev*, int);
using rtlsdr_set_tuner_gain_mode_fn = int (*)(rtlsdr_dev*, int);
using rtlsdr_set_tuner_gain_fn = int (*)(rtlsdr_dev*, int);
using rtlsdr_get_tuner_gains_fn = int (*)(rtlsdr_dev*, int*);
using rtlsdr_set_tuner_bandwidth_fn = int (*)(rtlsdr_dev*, std::uint32_t);
using rtlsdr_set_bias_tee_fn = int (*)(rtlsdr_dev*, int);
using rtlsdr_reset_buffer_fn = int (*)(rtlsdr_dev*);
using rtlsdr_read_async_fn = int (*)(rtlsdr_dev*, rtlsdr_read_async_cb_t, void*, std::uint32_t,
                                     std::uint32_t);
using rtlsdr_cancel_async_fn = int (*)(rtlsdr_dev*);

}  // namespace

// ---- SdrRing --------------------------------------------------------------------------------------

SdrRing::SdrRing(std::size_t capacity) : buffer_(std::max<std::size_t>(capacity, 1)) {}

void SdrRing::push(const std::complex<float>* samples, std::size_t count) {
    if (!samples || count == 0) return;
    const std::size_t cap = buffer_.size();
    std::lock_guard<std::mutex> lock(mutex_);
    if (count >= cap) {
        const std::size_t drop = count - cap;
        dropped_ += drop + size_;
        samples += drop;
        count = cap;
        head_ = 0;
        size_ = 0;
    }
    if (size_ + count > cap) {
        const std::size_t overflow = size_ + count - cap;
        head_ = (head_ + overflow) % cap;
        size_ -= overflow;
        dropped_ += overflow;
    }
    const std::size_t tail = (head_ + size_) % cap;
    const std::size_t first = std::min(count, cap - tail);
    std::memcpy(buffer_.data() + tail, samples, first * sizeof(std::complex<float>));
    if (count > first)
        std::memcpy(buffer_.data(), samples + first, (count - first) * sizeof(std::complex<float>));
    size_ += count;
    cv_.notify_all();
}

std::size_t SdrRing::pop(std::complex<float>* out, std::size_t maxCount, int timeoutMs,
                         const std::atomic<bool>& stop) {
    if (!out || maxCount == 0) return 0;
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(std::max(0, timeoutMs)),
                 [&] { return stop.load() || size_ > 0; });
    if (stop.load() || size_ == 0) return 0;
    const std::size_t n = std::min(maxCount, size_);
    const std::size_t cap = buffer_.size();
    const std::size_t first = std::min(n, cap - head_);
    std::memcpy(out, buffer_.data() + head_, first * sizeof(std::complex<float>));
    if (n > first) std::memcpy(out + first, buffer_.data(), (n - first) * sizeof(std::complex<float>));
    head_ = (head_ + n) % cap;
    size_ -= n;
    return n;
}

std::size_t SdrRing::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

std::uint64_t SdrRing::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void SdrRing::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
    cv_.notify_all();
}

// ---- SdrSource::Impl ------------------------------------------------------------------------------

class SdrSource::Impl {
public:
    ~Impl() { close(); }

    bool open(const SdrConfig& config, std::string& error);
    void close();
    bool isOpen() const { return open_; }
    std::uint64_t samplesReceived() const { return received_.load(); }
    std::uint64_t samplesDropped() const { return ring_.dropped(); }
    double sampleRate() const { return actualRate_; }
    double centerFrequency() const { return cfg_.centerFrequency; }

    std::size_t read(std::vector<std::complex<float>>& out, std::size_t maxFrames, int timeoutMs,
                     std::string& error) {
        if (!open_) {
            error = "SDR 没有打开。";
            return 0;
        }
        if (failed_.load()) {
            error = lastError();
            if (error.empty()) error = "SDR 接收已停止。";
            return 0;
        }
        out.resize(maxFrames);
        const std::size_t n = ring_.pop(out.data(), maxFrames, timeoutMs, stop_);
        out.resize(n);
        if (n == 0 && hackrf_ && hackrfIsStreaming_ && !stop_ &&
            std::chrono::steady_clock::now() - openTime_ > std::chrono::seconds(2)) {
            const int state = hackrfIsStreaming_(hackrf_);
            if (state != 1) {
                setError("HackRF 数据流中断（错误 " + std::to_string(state) +
                         "）；请更换 USB 口/线缆，确认供电与驱动。");
                error = lastError();
            }
        }
        return n;
    }

    void requestStop() {
        stop_.store(true);
        ring_.clear();
    }

    void setError(const std::string& error);

private:
    bool openHackRf(std::string& error);
    bool openRtlSdr(std::string& error);
    bool bindHackRf(std::string& error);
    bool bindRtlSdr(std::string& error);
    template <typename T>
    bool bind(T& fn, const char* name, std::string& error) {
        fn = reinterpret_cast<T>(lib_.symbol(name));
        if (!fn) {
            error = std::string("库中缺少函数 ") + name;
            return false;
        }
        return true;
    }
    std::string lastError() const;
    void handleSamples(const std::int8_t* samples, std::size_t frames);
    void handleSamples(const std::uint8_t* samples, std::size_t frames);
    static int hackrfCallback(hackrf_transfer* transfer);
    static void rtlsdrCallback(unsigned char* buf, std::uint32_t len, void* ctx);

    SdrConfig cfg_;
    bool open_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<bool> failed_{false};
    mutable std::mutex errorMutex_;
    std::string error_;
    double actualRate_ = 0.0;
    SdrRing ring_{kRingFrames};
    std::atomic<std::uint64_t> received_{0};
    std::vector<std::complex<float>> scratch_;
    DynamicLibrary lib_;

    // HackRF
    hackrf_device* hackrf_ = nullptr;
    bool hackrfInitialized_ = false;
    hackrf_init_fn hackrfInit_ = nullptr;
    hackrf_exit_fn hackrfExit_ = nullptr;
    hackrf_open_fn hackrfOpen_ = nullptr;
    hackrf_open_by_serial_fn hackrfOpenBySerial_ = nullptr;
    hackrf_close_fn hackrfClose_ = nullptr;
    hackrf_set_sample_rate_fn hackrfSetSampleRate_ = nullptr;
    hackrf_set_filter_fn hackrfSetFilter_ = nullptr;
    hackrf_compute_filter_fn hackrfComputeFilter_ = nullptr;
    hackrf_set_freq_fn hackrfSetFreq_ = nullptr;
    hackrf_set_gain_fn hackrfSetLnaGain_ = nullptr;
    hackrf_set_gain_fn hackrfSetVgaGain_ = nullptr;
    hackrf_set_amp_fn hackrfSetAmp_ = nullptr;
    hackrf_set_antenna_fn hackrfSetAntenna_ = nullptr;
    hackrf_start_rx_fn hackrfStartRx_ = nullptr;
    hackrf_stop_rx_fn hackrfStopRx_ = nullptr;
    hackrf_is_streaming_fn hackrfIsStreaming_ = nullptr;
    std::chrono::steady_clock::time_point openTime_{};
    hackrf_device_list_fn hackrfDeviceList_ = nullptr;
    hackrf_device_list_free_fn hackrfDeviceListFree_ = nullptr;

    // RTL-SDR
    rtlsdr_dev* rtl_ = nullptr;
    std::thread rtlThread_;
    rtlsdr_get_device_count_fn rtlGetDeviceCount_ = nullptr;
    rtlsdr_get_device_name_fn rtlGetDeviceName_ = nullptr;
    rtlsdr_get_device_usb_strings_fn rtlGetDeviceUsbStrings_ = nullptr;
    rtlsdr_open_fn rtlOpen_ = nullptr;
    rtlsdr_close_fn rtlClose_ = nullptr;
    rtlsdr_set_sample_rate_fn rtlSetSampleRate_ = nullptr;
    rtlsdr_get_sample_rate_fn rtlGetSampleRate_ = nullptr;
    rtlsdr_set_center_freq_fn rtlSetCenterFreq_ = nullptr;
    rtlsdr_set_freq_correction_fn rtlSetFreqCorrection_ = nullptr;
    rtlsdr_set_agc_mode_fn rtlSetAgcMode_ = nullptr;
    rtlsdr_set_tuner_gain_mode_fn rtlSetGainMode_ = nullptr;
    rtlsdr_set_tuner_gain_fn rtlSetGain_ = nullptr;
    rtlsdr_get_tuner_gains_fn rtlGetGains_ = nullptr;
    rtlsdr_set_tuner_bandwidth_fn rtlSetBandwidth_ = nullptr;
    rtlsdr_set_bias_tee_fn rtlSetBiasTee_ = nullptr;
    rtlsdr_reset_buffer_fn rtlResetBuffer_ = nullptr;
    rtlsdr_read_async_fn rtlReadAsync_ = nullptr;
    rtlsdr_cancel_async_fn rtlCancelAsync_ = nullptr;
};

bool SdrSource::Impl::open(const SdrConfig& config, std::string& error) {
    close();
    cfg_ = config;
    stop_.store(false);
    failed_.store(false);
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_.clear();
    }
    received_.store(0);
    ring_.clear();
    actualRate_ = 0.0;

    if (config.sampleRate < 1.0e6 || config.sampleRate > 20.0e6) {
        error = "采样率必须在 1–20 MS/s 之间。";
        return false;
    }
    if (config.centerFrequency < 1.0e6 || config.centerFrequency > 6.0e9) {
        error = "中心频率超出范围。";
        return false;
    }
    const bool ok = config.kind == SdrKind::HackRf ? openHackRf(error) : openRtlSdr(error);
    if (!ok) {
        close();
        return false;
    }
    open_ = true;
    return true;
}

bool SdrSource::Impl::bindHackRf(std::string& error) {
    return bind(hackrfInit_, "hackrf_init", error) && bind(hackrfExit_, "hackrf_exit", error) &&
           bind(hackrfOpen_, "hackrf_open", error) && bind(hackrfClose_, "hackrf_close", error) &&
           bind(hackrfSetSampleRate_, "hackrf_set_sample_rate", error) &&
           bind(hackrfSetFreq_, "hackrf_set_freq", error) &&
           bind(hackrfSetLnaGain_, "hackrf_set_lna_gain", error) &&
           bind(hackrfSetVgaGain_, "hackrf_set_vga_gain", error) &&
           bind(hackrfSetAmp_, "hackrf_set_amp_enable", error) &&
           bind(hackrfStartRx_, "hackrf_start_rx", error) &&
           bind(hackrfStopRx_, "hackrf_stop_rx", error);
}

bool SdrSource::Impl::openHackRf(std::string& error) {
    if (!lib_.open(hackrfLibraryNames(), error)) return false;
    if (!bindHackRf(error)) return false;
    hackrfOpenBySerial_ =
        reinterpret_cast<hackrf_open_by_serial_fn>(lib_.symbol("hackrf_open_by_serial"));
    hackrfIsStreaming_ =
        reinterpret_cast<hackrf_is_streaming_fn>(lib_.symbol("hackrf_is_streaming"));
    hackrfSetFilter_ =
        reinterpret_cast<hackrf_set_filter_fn>(lib_.symbol("hackrf_set_baseband_filter_bandwidth"));
    hackrfComputeFilter_ =
        reinterpret_cast<hackrf_compute_filter_fn>(lib_.symbol("hackrf_compute_baseband_filter_bw"));
    hackrfSetAntenna_ =
        reinterpret_cast<hackrf_set_antenna_fn>(lib_.symbol("hackrf_set_antenna_enable"));
    hackrfDeviceList_ =
        reinterpret_cast<hackrf_device_list_fn>(lib_.symbol("hackrf_device_list"));
    hackrfDeviceListFree_ =
        reinterpret_cast<hackrf_device_list_free_fn>(lib_.symbol("hackrf_device_list_free"));

    if (hackrfInit_() != 0) {
        error = "HackRF 初始化失败（检查驱动与 USB 连接）。";
        return false;
    }
    hackrfInitialized_ = true;
    int ret = 0;
    if (!cfg_.serial.empty() && hackrfOpenBySerial_)
        ret = hackrfOpenBySerial_(cfg_.serial.c_str(), &hackrf_);
    else
        ret = hackrfOpen_(&hackrf_);
    if (ret != 0 || !hackrf_) {
        error = "未找到 HackRF 设备（检查驱动与 USB 连接）。";
        return false;
    }

    const double rate = std::min(cfg_.sampleRate, 20.0e6);
    if (hackrfSetSampleRate_(hackrf_, rate) != 0) {
        error = "HackRF 设置采样率失败。";
        return false;
    }
    actualRate_ = rate;
    if (hackrfSetFilter_) {
        std::uint32_t bw = static_cast<std::uint32_t>(std::lround(rate));
        if (hackrfSetFilter_(hackrf_, bw) != 0 && hackrfComputeFilter_) {
            const std::uint32_t computed = hackrfComputeFilter_(bw);
            if (computed > 0) hackrfSetFilter_(hackrf_, computed);
        }
    }
    if (hackrfSetFreq_(hackrf_, static_cast<std::uint64_t>(cfg_.centerFrequency)) != 0) {
        error = "HackRF 无法调到 " + std::to_string(cfg_.centerFrequency / 1e6) + " MHz。";
        return false;
    }
    const int lna = std::clamp(static_cast<int>(std::lround(cfg_.lnaGainDb / 8.0) * 8), 0, 40);
    const int vga = std::clamp(static_cast<int>(std::lround(cfg_.vgaGainDb / 2.0) * 2), 0, 62);
    hackrfSetLnaGain_(hackrf_, static_cast<std::uint32_t>(lna));
    hackrfSetVgaGain_(hackrf_, static_cast<std::uint32_t>(vga));
    hackrfSetAmp_(hackrf_, cfg_.amplifier ? 1 : 0);
    if (hackrfSetAntenna_) hackrfSetAntenna_(hackrf_, cfg_.biasTee ? 1 : 0);

    if (hackrfStartRx_(hackrf_, &Impl::hackrfCallback, this) != 0) {
        error = "HackRF 启动接收失败。";
        return false;
    }
    openTime_ = std::chrono::steady_clock::now();
    return true;
}

bool SdrSource::Impl::bindRtlSdr(std::string& error) {
    return bind(rtlGetDeviceCount_, "rtlsdr_get_device_count", error) &&
           bind(rtlGetDeviceName_, "rtlsdr_get_device_name", error) &&
           bind(rtlOpen_, "rtlsdr_open", error) && bind(rtlClose_, "rtlsdr_close", error) &&
           bind(rtlSetSampleRate_, "rtlsdr_set_sample_rate", error) &&
           bind(rtlGetSampleRate_, "rtlsdr_get_sample_rate", error) &&
           bind(rtlSetCenterFreq_, "rtlsdr_set_center_freq", error) &&
           bind(rtlSetFreqCorrection_, "rtlsdr_set_freq_correction", error) &&
           bind(rtlSetGainMode_, "rtlsdr_set_tuner_gain_mode", error) &&
           bind(rtlSetGain_, "rtlsdr_set_tuner_gain", error) &&
           bind(rtlGetGains_, "rtlsdr_get_tuner_gains", error) &&
           bind(rtlResetBuffer_, "rtlsdr_reset_buffer", error) &&
           bind(rtlReadAsync_, "rtlsdr_read_async", error) &&
           bind(rtlCancelAsync_, "rtlsdr_cancel_async", error);
}

bool SdrSource::Impl::openRtlSdr(std::string& error) {
    if (!lib_.open(rtlsdrLibraryNames(), error)) return false;
    if (!bindRtlSdr(error)) return false;
    rtlGetDeviceUsbStrings_ = reinterpret_cast<rtlsdr_get_device_usb_strings_fn>(
        lib_.symbol("rtlsdr_get_device_usb_strings"));
    rtlSetAgcMode_ = reinterpret_cast<rtlsdr_set_agc_mode_fn>(lib_.symbol("rtlsdr_set_agc_mode"));
    rtlSetBandwidth_ =
        reinterpret_cast<rtlsdr_set_tuner_bandwidth_fn>(lib_.symbol("rtlsdr_set_tuner_bandwidth"));
    rtlSetBiasTee_ = reinterpret_cast<rtlsdr_set_bias_tee_fn>(lib_.symbol("rtlsdr_set_bias_tee"));

    const std::uint32_t count = rtlGetDeviceCount_();
    if (count == 0) {
        error = "未找到 RTL-SDR 设备（检查 WinUSB 驱动与 USB 连接）。";
        return false;
    }
    const int index = std::clamp(cfg_.deviceIndex, 0, static_cast<int>(count) - 1);
    if (rtlOpen_(&rtl_, static_cast<std::uint32_t>(index)) != 0 || !rtl_) {
        error = "无法打开 RTL-SDR 设备（检查驱动是否被其他程序占用）。";
        return false;
    }
    if (cfg_.sampleRate > 3.2e6 + 1.0) {
        error = "RTL-SDR 最高支持 3.2 MS/s；请把采样率调到 3.2 或更低。";
        return false;
    }
    const std::uint32_t rate = static_cast<std::uint32_t>(std::lround(cfg_.sampleRate));
    if (rtlSetSampleRate_(rtl_, rate) != 0) {
        error = "RTL-SDR 设置采样率失败。";
        return false;
    }
    actualRate_ = static_cast<double>(rtlGetSampleRate_(rtl_));
    if (actualRate_ <= 0.0) actualRate_ = cfg_.sampleRate;
    if (rtlSetCenterFreq_(rtl_, static_cast<std::uint32_t>(std::lround(cfg_.centerFrequency))) < 0) {
        error = "RTL-SDR 无法调到 " + std::to_string(cfg_.centerFrequency / 1e6) + " MHz。";
        return false;
    }
    rtlSetFreqCorrection_(rtl_, std::clamp(cfg_.ppm, -1000, 1000));
    if (rtlSetBandwidth_) rtlSetBandwidth_(rtl_, 0);   // 0 = automatic
    if (rtlSetBiasTee_) rtlSetBiasTee_(rtl_, cfg_.biasTee ? 1 : 0);

    if (rtlSetAgcMode_) rtlSetAgcMode_(rtl_, cfg_.autoGain ? 1 : 0);
    rtlSetGainMode_(rtl_, cfg_.autoGain ? 0 : 1);
    if (!cfg_.autoGain) {
        const int n = rtlGetGains_(rtl_, nullptr);
        if (n > 0) {
            std::vector<int> gains(static_cast<std::size_t>(n));
            if (rtlGetGains_(rtl_, gains.data()) == n) {
                std::sort(gains.begin(), gains.end());
                const int want = static_cast<int>(std::lround(cfg_.gainDb * 10.0));
                auto it = std::lower_bound(gains.begin(), gains.end(), want);
                if (it == gains.end()) --it;
                int best = *it;
                if (it != gains.begin() && std::abs(*std::prev(it) - want) < std::abs(best - want))
                    best = *std::prev(it);
                for (int attempt = 0; attempt < 10 && rtlSetGain_(rtl_, best) < 0; ++attempt) { }
            }
        }
    }
    rtlResetBuffer_(rtl_);
    rtlThread_ = std::thread([this] {
        const int ret = rtlReadAsync_(rtl_, &Impl::rtlsdrCallback, this, 0, 0);
        if (ret != 0 && !stop_.load()) setError("RTL-SDR 接收线程异常退出（" + std::to_string(ret) + "）。");
    });
    return true;
}

void SdrSource::Impl::close() {
    stop_.store(true);
    ring_.clear();
    if (hackrf_) {
        if (hackrfSetAntenna_) hackrfSetAntenna_(hackrf_, 0);
        if (hackrfStopRx_) hackrfStopRx_(hackrf_);
        if (hackrfClose_) hackrfClose_(hackrf_);
        hackrf_ = nullptr;
    }
    if (hackrfInitialized_ && hackrfExit_) {
        hackrfExit_();
        hackrfInitialized_ = false;
    }
    if (rtl_) {
        if (rtlSetBiasTee_) rtlSetBiasTee_(rtl_, 0);
        if (rtlCancelAsync_) rtlCancelAsync_(rtl_);
        if (rtlThread_.joinable()) rtlThread_.join();
        if (rtlClose_) rtlClose_(rtl_);
        rtl_ = nullptr;
    } else if (rtlThread_.joinable()) {
        rtlThread_.join();
    }
    lib_.close();
    open_ = false;
    actualRate_ = 0.0;
}

void SdrSource::Impl::setError(const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(errorMutex_);
        if (error_.empty()) error_ = error;
    }
    failed_.store(true);
    ring_.clear();
}

std::string SdrSource::Impl::lastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return error_;
}

void SdrSource::Impl::handleSamples(const std::int8_t* samples, std::size_t frames) {
    scratch_.resize(frames);
    for (std::size_t i = 0; i < frames; ++i)
        scratch_[i] = {static_cast<float>(samples[2 * i]) / 128.0f,
                       static_cast<float>(samples[2 * i + 1]) / 128.0f};
    ring_.push(scratch_.data(), frames);
    received_.fetch_add(frames);
}

void SdrSource::Impl::handleSamples(const std::uint8_t* samples, std::size_t frames) {
    scratch_.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        // Match the usual RTL-SDR mid-scale and full scale closely enough for the AGC.
        const float re = (static_cast<float>(samples[2 * i]) - 127.4f) / 128.0f;
        const float im = (static_cast<float>(samples[2 * i + 1]) - 127.4f) / 128.0f;
        scratch_[i] = {re, im};
    }
    ring_.push(scratch_.data(), frames);
    received_.fetch_add(frames);
}

int SdrSource::Impl::hackrfCallback(hackrf_transfer* transfer) {
    if (!transfer || !transfer->rx_ctx || !transfer->buffer) return 0;
    auto* self = static_cast<Impl*>(transfer->rx_ctx);
    if (self->stop_.load()) return 0;
    std::uint32_t bytes = transfer->buffer_length;
    if (transfer->valid_length > 0 && static_cast<std::uint32_t>(transfer->valid_length) < bytes)
        bytes = static_cast<std::uint32_t>(transfer->valid_length);
    self->handleSamples(reinterpret_cast<const std::int8_t*>(transfer->buffer),
                        static_cast<std::size_t>(bytes) / 2);
    return 0;
}

void SdrSource::Impl::rtlsdrCallback(unsigned char* buf, std::uint32_t len, void* ctx) {
    if (!buf || !ctx) return;
    auto* self = static_cast<Impl*>(ctx);
    if (self->stop_.load()) return;
    self->handleSamples(reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(len) / 2);
}

// ---- SdrSource public API -------------------------------------------------------------------------

SdrSource::SdrSource() : impl_(std::make_unique<Impl>()) {}
SdrSource::~SdrSource() = default;

bool SdrSource::open(const SdrConfig& config, std::string& error) { return impl_->open(config, error); }
void SdrSource::close() { impl_->close(); }
bool SdrSource::isOpen() const { return impl_->isOpen(); }
double SdrSource::sampleRate() const { return impl_->sampleRate(); }
double SdrSource::centerFrequency() const { return impl_->centerFrequency(); }
std::uint64_t SdrSource::samplesReceived() const { return impl_->samplesReceived(); }
std::uint64_t SdrSource::samplesDropped() const { return impl_->samplesDropped(); }

std::size_t SdrSource::read(std::vector<std::complex<float>>& out, std::size_t maxFrames, int timeoutMs,
                            std::string& error) {
    return impl_->read(out, maxFrames, timeoutMs, error);
}
void SdrSource::requestStop() { impl_->requestStop(); }

std::vector<SdrDeviceInfo> SdrSource::enumerate(SdrKind kind, std::string& error) {
    error.clear();
    std::vector<SdrDeviceInfo> devices;
    DynamicLibrary lib;
    if (kind == SdrKind::HackRf) {
        if (!lib.open(hackrfLibraryNames(), error)) return devices;
        auto initFn = reinterpret_cast<hackrf_init_fn>(lib.symbol("hackrf_init"));
        auto exitFn = reinterpret_cast<hackrf_exit_fn>(lib.symbol("hackrf_exit"));
        auto listFn = reinterpret_cast<hackrf_device_list_fn>(lib.symbol("hackrf_device_list"));
        auto freeFn = reinterpret_cast<hackrf_device_list_free_fn>(lib.symbol("hackrf_device_list_free"));
        const int init = initFn ? initFn() : -1;
        if (init != 0) {
            error = "HackRF 初始化失败（错误 " + std::to_string(init) + "）；检查驱动、USB 连接，"
                    "并关闭其他占用设备的程序。";
            return devices;
        }
        bool listed = false;
        if (listFn && freeFn) {
            hackrf_device_list* list = listFn();
            if (list) {
                listed = true;
                for (int i = 0; i < list->devicecount; ++i) {
                    const char* serial = list->serial_numbers ? list->serial_numbers[i] : nullptr;
                    std::string id = serial ? serial : "";
                    std::string name = id.empty() ? "HackRF One [占用]" : "HackRF One " + id;
                    devices.push_back({kind, i, id, name});
                }
                freeFn(list);
            }
        }
        if (exitFn) exitFn();
        if (listed && devices.empty())
            error = "未找到 HackRF 设备（检查驱动与 USB 连接，并关闭其他占用设备的程序）。";
        else if (!listed && devices.empty())
            devices.push_back({kind, 0, "", "HackRF（第一个可用设备）"});
        return devices;
    }

    if (!lib.open(rtlsdrLibraryNames(), error)) return devices;
    auto countFn = reinterpret_cast<rtlsdr_get_device_count_fn>(lib.symbol("rtlsdr_get_device_count"));
    auto nameFn = reinterpret_cast<rtlsdr_get_device_name_fn>(lib.symbol("rtlsdr_get_device_name"));
    auto usbFn = reinterpret_cast<rtlsdr_get_device_usb_strings_fn>(
        lib.symbol("rtlsdr_get_device_usb_strings"));
    if (!countFn) {
        error = "librtlsdr 缺少 rtlsdr_get_device_count。";
        return devices;
    }
    const std::uint32_t count = countFn();
    if (count == 0) {
        error = "未找到 RTL-SDR 设备（检查 WinUSB 驱动与 USB 连接）。";
        return devices;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string name = nameFn ? nameFn(i) : "RTL-SDR";
        std::string serial;
        if (usbFn) {
            char manufact[256] = {}, product[256] = {}, serialBuf[256] = {};
            if (usbFn(i, manufact, product, serialBuf) == 0) {
                if (product[0]) name = std::string(manufact) + " " + product;
                serial = serialBuf;
            }
        }
        if (name.empty()) name = "RTL-SDR";
        name += " #" + std::to_string(i);
        devices.push_back({kind, static_cast<int>(i), serial, name});
    }
    return devices;
}

const char* sdrKindName(SdrKind kind) {
    return kind == SdrKind::HackRf ? "HackRF" : "RTL-SDR";
}

std::string sdrKindDescription(SdrKind kind) {
    return kind == SdrKind::HackRf ? "HackRF（USB 接收）" : "RTL-SDR（USB 接收，最高 3.2 MS/s）";
}

bool sdrSampleRateOk(double sampleRate) { return sampleRate >= 3.15e6; }

double sdrMaxSampleRate(SdrKind kind) { return kind == SdrKind::HackRf ? 20.0e6 : 3.2e6; }

double sdrDefaultSampleRate(SdrKind kind) { return kind == SdrKind::HackRf ? 6.0e6 : 3.2e6; }

}  // namespace hritapp
