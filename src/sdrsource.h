// sdrsource.h - receive-only I/Q sources for HackRF and RTL-SDR.
//
// The libraries are not build or link dependencies: they are loaded at run time from the program
// directory (or PATH), so a build without them still works and the program starts normally. Only
// the receive path is used; there is no transmit code.
//
// Both backends deliver interleaved I/Q samples to a bounded ring, which decouples the USB callback
// from the decoder. If the decoder falls behind, the oldest samples are dropped and counted instead
// of stalling the device.
#pragma once
#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hritapp {

enum class SdrKind { HackRf, RtlSdr };

struct SdrDeviceInfo {
    SdrKind kind = SdrKind::HackRf;
    int index = 0;
    std::string serial;   // HackRF serial; empty when the backend has no serial
    std::string name;
};

struct SdrConfig {
    SdrKind kind = SdrKind::HackRf;
    int deviceIndex = 0;
    std::string serial;         // HackRF: open by serial; empty = first device
    double sampleRate = 6.0e6;
    double centerFrequency = 1695.4e6;
    double gainDb = 40.0;       // RTL-SDR tuner gain, tenths of a dB are chosen by the library
    int lnaGainDb = 32;         // HackRF LNA (0..40, step 8)
    int vgaGainDb = 40;         // HackRF VGA (0..62, step 2)
    bool amplifier = false;     // HackRF RF amplifier (+14 dB)
    bool autoGain = true;       // RTL-SDR AGC / automatic tuner gain
    bool biasTee = false;       // RTL-SDR bias tee, HackRF antenna power
    int ppm = 0;                // RTL-SDR frequency correction
};

const char* sdrKindName(SdrKind kind);
// "HackRF（USB，6 MS/s 起）" etc., for the window and the log.
std::string sdrKindDescription(SdrKind kind);
// Whether the requested rate can carry the 3 Msym/s HRIT link (>= 3.15 MS/s).
bool sdrSampleRateOk(double sampleRate);
// Highest rate the backend can be asked for (RTL-SDR is limited to 3.2 MS/s).
double sdrMaxSampleRate(SdrKind kind);
// Default rate to put in the window for a backend.
double sdrDefaultSampleRate(SdrKind kind);

// Bounded FIFO of complex samples, fed by the device callback and drained by the decoder.
// push() never blocks: when full it discards the oldest samples and counts them.
class SdrRing {
public:
    explicit SdrRing(std::size_t capacity);
    void push(const std::complex<float>* samples, std::size_t count);
    // Waits until samples are available or timeoutMs/stop. Returns the number copied (0 on timeout).
    std::size_t pop(std::complex<float>* out, std::size_t maxCount, int timeoutMs,
                    const std::atomic<bool>& stop);
    std::size_t size() const;
    std::uint64_t dropped() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::complex<float>> buffer_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t dropped_ = 0;
};

class SdrSource {
public:
    SdrSource();
    ~SdrSource();
    SdrSource(const SdrSource&) = delete;
    SdrSource& operator=(const SdrSource&) = delete;

    // Loads the backend library and lists devices. An empty list with an error is the normal result
    // when the library or the driver is not installed.
    static std::vector<SdrDeviceInfo> enumerate(SdrKind kind, std::string& error);

    bool open(const SdrConfig& config, std::string& error);
    void close();
    bool isOpen() const;
    double sampleRate() const;
    double centerFrequency() const;
    std::uint64_t samplesReceived() const;
    std::uint64_t samplesDropped() const;

    // Blocking read with a timeout so a stop request is noticed quickly. Returns 0 on timeout or
    // when no samples arrived; `error` is non-empty only when the device failed.
    std::size_t read(std::vector<std::complex<float>>& out, std::size_t maxFrames, int timeoutMs,
                     std::string& error);
    void requestStop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hritapp
