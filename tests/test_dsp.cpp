// test_dsp.cpp - checks the DC blocker used ahead of the QPSK demodulator.
// The blocker must remove a constant offset (LO leakage / ADC offset / SDR centre spike) without
// touching a signal that is hundreds of kilohertz away from DC.
#include "dsp.h"

#include <cmath>
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

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

int main() {
    using nadirstation::dsp::DcBlocker;
    using nadirstation::dsp::PskDemod;

    // A constant offset must disappear, and the estimate must converge to it.
    {
        DcBlocker blocker;
        blocker.configure(6000000.0f, 500.0f);
        const std::complex<float> offset{0.35f, -0.2f};
        std::complex<float> last{0.0f, 0.0f};
        for (int i = 0; i < 600000; ++i) last = blocker.process(offset);
        check(std::abs(last) < 1.0e-4f, "constant offset is removed");
        check(std::abs(blocker.estimate() - offset) < 1.0e-4f, "constant offset estimate");
    }

    // A 1 MHz tone is far above the 500 Hz corner: its amplitude must be preserved.
    {
        DcBlocker blocker;
        blocker.configure(6000000.0f, 500.0f);
        const std::complex<float> offset{-0.1f, 0.25f};
        double magnitudeSum = 0.0;
        std::size_t count = 0;
        for (int i = 0; i < 600000; ++i) {
            const float phase = 2.0f * kPi * 1.0e6f * static_cast<float>(i) / 6000000.0f;
            const std::complex<float> tone{std::cos(phase), std::sin(phase)};
            const std::complex<float> out = blocker.process(offset + tone);
            if (i > 100000) {
                magnitudeSum += std::abs(out);
                ++count;
            }
        }
        const double meanMagnitude = magnitudeSum / static_cast<double>(count);
        check(std::fabs(meanMagnitude - 1.0) < 0.005, "1 MHz tone amplitude preserved");
        check(std::abs(blocker.estimate() - offset) < 0.01f, "tone does not bias the estimate");
    }

    // The DC estimate of a QPSK-like zero-mean signal must stay near zero when the signal has no
    // offset, so the blocker does not eat the wanted signal.
    {
        DcBlocker blocker;
        blocker.configure(6000000.0f, 500.0f);
        std::complex<float> last{0.0f, 0.0f};
        for (int i = 0; i < 600000; ++i) {
            const std::complex<float> x = (i & 1) ? std::complex<float>{0.7071f, 0.7071f}
                                                  : std::complex<float>{-0.7071f, -0.7071f};
            last = blocker.process(x);
        }
        check(std::abs(blocker.estimate()) < 1.0e-3f, "zero-mean signal keeps a zero estimate");
        check(std::abs(last) > 0.5f, "zero-mean signal is not removed");
    }

    // Invalid parameters must fall back to the default and keep working.
    {
        DcBlocker blocker;
        blocker.configure(0.0f, 0.0f);
        for (int i = 0; i < 1000; ++i) blocker.process({1.0f, 1.0f});
        check(std::isfinite(blocker.estimate().real()) && std::isfinite(blocker.estimate().imag()),
              "invalid parameters fall back safely");
    }

    // A strong unmodulated carrier far outside the plausible L-band offset must not pull the
    // fourth-power acquisition across the band: the reported offset is capped at 250 kHz.
    {
        PskDemod demod;
        demod.setQpsk(true);
        demod.setRectangularPulse(true);
        demod.configure(6000000.0f, 3000000.0f, 0.5f, 0.02f);
        std::vector<std::complex<float>> iq(131072);
        for (std::size_t i = 0; i < iq.size(); ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * 700000.0 * static_cast<double>(i) / 6000000.0;
            iq[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
        const auto result = demod.process(iq);
        check(std::fabs(result.carrierOffsetHz) <= 251000.0f, "CW carrier is not acquired beyond 250 kHz");
    }

    // A deliberately huge initial offset is clamped when the loop is reset.
    {
        PskDemod demod;
        demod.setQpsk(true);
        demod.setRectangularPulse(true);
        demod.configure(6000000.0f, 3000000.0f, 0.5f, 0.02f);
        demod.setInitialCarrierOffset(1.0f);   // radians/sample, far beyond the allowed range
        demod.reset();
        check(std::fabs(demod.carrierOffsetHz()) <= 251000.0f, "initial carrier offset is clamped");
    }

    // A user-supplied starting offset restricts acquisition to +/-50 kHz around it, even when a
    // stronger CW sits elsewhere. This is the escape hatch for a known offset or a false lock.
    {
        PskDemod demod;
        demod.setQpsk(true);
        demod.setRectangularPulse(true);
        demod.setCarrierSearch(-100000.0f, 50000.0f);
        demod.configure(6000000.0f, 3000000.0f, 0.5f, 0.02f);
        std::vector<std::complex<float>> iq(131072);
        for (std::size_t i = 0; i < iq.size(); ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * 633851.0 * static_cast<double>(i) / 6000000.0;
            iq[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        }
        const auto result = demod.process(iq);
        check(result.carrierOffsetHz < -30000.0f && result.carrierOffsetHz > -170000.0f,
              "user carrier hint is honoured instead of the CW");
    }

    if (g_failures == 0) std::printf("all dsp tests passed\n");
    else std::fprintf(stderr, "%d dsp test(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
