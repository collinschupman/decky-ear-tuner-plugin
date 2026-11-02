#pragma once

#include <array>
#include <cstddef>
#include <utility>

/**
 * Applies independent wide-dynamic-range compression to frequency bands.
 *
 * The input is split into bands, each band is processed by its own compressor,
 * and the compressed samples are summed to form the mono output.
 *
 * @tparam BandCount Number of frequency bands.
 * @tparam BandSplitter Splitter whose output contains BandCount samples.
 * @tparam BandCompressor Compressor type with SetSampleRate, SetParameters,
 *                        and Process methods.
 */
template <size_t BandCount, typename BandSplitter, typename BandCompressor>
class MultiBandWideDynamicRangeCompressor {
   public:
    /** Creates a processor using the splitter's default configuration. */
    MultiBandWideDynamicRangeCompressor() noexcept
        : mSplitter()
    {
    }

    /**
     * Creates a processor with explicit crossover frequencies.
     *
     * @param crossoverFrequencies Crossover frequencies accepted by the
     *                             splitter, typically in Hz.
     */
    explicit MultiBandWideDynamicRangeCompressor(
        const typename BandSplitter::CrossoverFrequencies &crossoverFrequencies) noexcept
        : mSplitter(crossoverFrequencies)
    {
    }

    /** Forwards constructor arguments to the splitter. */
    template <typename... SplitterArguments>
    explicit MultiBandWideDynamicRangeCompressor(SplitterArguments &&...splitterArguments) noexcept
        : mSplitter(std::forward<SplitterArguments>(splitterArguments)...)
    {
    }

    /**
     * Updates the sample rate of the splitter and every band compressor.
     *
     * @param sampleRate Audio sample rate in samples per second.
     */
    void SetSampleRate(float sampleRate) noexcept
    {
        mSplitter.SetSampleRate(sampleRate);
        for (auto &compressor : mCompressors) {
            compressor.SetSampleRate(sampleRate);
        }
    }

    /**
     * Sets the per-band maximum gain and shared threshold.
     *
     * @param maxGainsDb Maximum gain in dB for each band.
     * @param threshold Shared threshold passed to each band compressor; its
     *                  units and valid range are defined by BandCompressor.
     */
    void SetParameters(const std::array<float, BandCount> &maxGainsDb, float threshold) noexcept
    {
        for (size_t band = 0; band < mCompressors.size(); ++band) {
            mCompressors[band].SetParameters(maxGainsDb[band], threshold);
        }
    }

    /**
     * Processes one mono input sample through the multiband compressor.
     *
     * @param input Input sample.
     * @return Sum of the processed band samples.
     */
    float Process(float input) noexcept
    {
        const std::array<float, BandCount> bands = mSplitter.Process(input);
        float output = 0.0f;
        for (size_t band = 0; band < bands.size(); ++band) {
            output += mCompressors[band].Process(bands[band]);
        }
        return output;
    }

   private:
    BandSplitter mSplitter;
    std::array<BandCompressor, BandCount> mCompressors;
};

/** Convenience alias for a four-band compressor. */
template <typename BandSplitter, typename BandCompressor>
using FourBandWideDynamicRangeCompressor =
    MultiBandWideDynamicRangeCompressor<4, BandSplitter, BandCompressor>;