// test_sdr.cpp - checks the live-source plumbing that can run without radio hardware:
// the bounded sample ring and the "library not installed" path of the device enumeration.
#include "sdrsource.h"

#include <atomic>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::complex<float> sample(int i) { return {static_cast<float>(i), -static_cast<float>(i)}; }

}  // namespace

int main() {
    using hritapp::SdrKind;
    using hritapp::SdrRing;

    {
        SdrRing ring(8);
        std::atomic<bool> stop{false};
        std::vector<std::complex<float>> in;
        for (int i = 0; i < 6; ++i) in.push_back(sample(i));
        ring.push(in.data(), in.size());
        check(ring.size() == 6, "ring size after push");
        std::vector<std::complex<float>> out(3);
        check(ring.pop(out.data(), 3, 0, stop) == 3, "ring pop count");
        check(out[0] == sample(0) && out[2] == sample(2), "ring FIFO order");
        check(ring.size() == 3, "ring size after pop");

        std::vector<std::complex<float>> more;
        for (int i = 6; i < 12; ++i) more.push_back(sample(i));
        ring.push(more.data(), more.size());   // 3 + 6 > 8: the oldest sample is dropped
        check(ring.dropped() == 1, "ring drop count");
        std::vector<std::complex<float>> all(8);
        check(ring.pop(all.data(), 8, 0, stop) == 8, "ring pop all");
        check(all[0] == sample(4), "ring drop keeps newest samples");
        check(all[7] == sample(11), "ring newest sample");
        check(ring.size() == 0, "ring empty after pop");
    }

    {
        SdrRing ring(4);
        std::atomic<bool> stop{false};
        std::vector<std::complex<float>> in;
        for (int i = 0; i < 6; ++i) in.push_back(sample(i));
        ring.push(in.data(), in.size());   // larger than the capacity: only the last four fit
        check(ring.dropped() == 2, "ring oversized push drop count");
        std::vector<std::complex<float>> out(4);
        check(ring.pop(out.data(), 4, 0, stop) == 4 && out[0] == sample(2) && out[3] == sample(5),
              "ring oversized push keeps the tail");
    }

    {
        SdrRing ring(4);
        std::atomic<bool> stop{false};
        std::vector<std::complex<float>> out(4);
        check(ring.pop(out.data(), 4, 10, stop) == 0, "ring timeout on empty");
        stop.store(true);
        check(ring.pop(out.data(), 4, 0, stop) == 0, "ring stop returns nothing");
    }

    {
        check(hritapp::sdrSampleRateOk(3.2e6), "3.2 MS/s accepted");
        check(!hritapp::sdrSampleRateOk(2.4e6), "2.4 MS/s rejected");
        check(hritapp::sdrDefaultSampleRate(SdrKind::HackRf) >= 3.15e6, "HackRF default rate");
        check(hritapp::sdrDefaultSampleRate(SdrKind::RtlSdr) >= 3.15e6, "RTL-SDR default rate");
        check(hritapp::sdrMaxSampleRate(SdrKind::RtlSdr) <= 3.2e6, "RTL-SDR maximum rate");
    }

    {
        // Without a driver/library this is the normal path; with one installed it may list devices.
        // Either way it must not crash and must return a clear error when the list is empty.
        std::string error;
        const auto devices = hritapp::SdrSource::enumerate(SdrKind::HackRf, error);
        check(!devices.empty() || !error.empty(), "HackRF enumeration reports a reason");
        error.clear();
        const auto rtl = hritapp::SdrSource::enumerate(SdrKind::RtlSdr, error);
        check(!rtl.empty() || !error.empty(), "RTL-SDR enumeration reports a reason");
    }

    if (g_failures == 0) std::printf("all sdr tests passed\n");
    else std::fprintf(stderr, "%d sdr test(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
