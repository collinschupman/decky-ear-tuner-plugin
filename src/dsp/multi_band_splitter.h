#pragma once

#include <array>
#include <cstddef>
#include <utility>

/**
 * Splits a mono input into sequential frequency bands using cascaded filters.
 *
 * Each filter processes the remainder left after the preceding bands have
 * been removed. The final band is that remainder, so the bands sum to the
 * input apart from floating-point rounding.
 *
 * @tparam BandCount Number of output bands; must be at least two.
 * @tparam BandFilter Stateful filter type constructible from a cutoff in Hz,
 *                    with SetSampleRate(float) and Process(float) methods.
 */
template <size_t BandCount, typename BandFilter>
class MultiBandSplitter {
    static_assert(BandCount >= 2);

   public:
    /** Cutoff frequencies in Hz, one for each filter/band boundary. */
    using CrossoverFrequencies = std::array<float, BandCount - 1>;

    /**
     * Creates the splitter with explicit or default crossover frequencies.
     *
     * For four bands the defaults are 250, 1000, and 4000 Hz; otherwise they
     * are 250 Hz multiples in ascending order. Frequencies are passed to the
     * filters as supplied and are not validated or reordered.
     */
    explicit MultiBandSplitter(
        const CrossoverFrequencies &crossoverFrequencies = DefaultCrossoverFrequencies()) noexcept
        : mFilters(MakeFilters(crossoverFrequencies, std::make_index_sequence<BandCount - 1>{}))
    {
    }

    /**
     * Updates the sample rate of every band filter.
     *
     * Call this before processing so filters can calculate their coefficients
     * for the active sample rate.
     *
     * @param sampleRate Audio sample rate in samples per second.
     */
    void SetSampleRate(float sampleRate) noexcept
    {
        for (auto &filter : mFilters) {
            filter.SetSampleRate(sampleRate);
        }
    }

    /**
     * Splits one mono sample into its frequency bands.
     *
     * @param input Input sample.
     * @return One sample per band, from lowest to highest frequency.
     */
    std::array<float, BandCount> Process(float input) noexcept
    {
        std::array<float, BandCount> bands{};
        float remainder = input;
        for (size_t band = 0; band < BandCount - 1; ++band) {
            bands[band] = mFilters[band].Process(remainder);
            remainder -= bands[band];
        }
        bands[BandCount - 1] = remainder;
        return bands;
    }

   private:
    static constexpr CrossoverFrequencies DefaultCrossoverFrequencies()
    {
        CrossoverFrequencies frequencies{};
        if constexpr (BandCount == 4) {
            frequencies = {250.0f, 1000.0f, 4000.0f};
            return frequencies;
        }
        for (size_t band = 0; band < BandCount - 1; ++band) {
            frequencies[band] = 250.0f * static_cast<float>(band + 1);
        }
        return frequencies;
    }

    template <size_t... Bands>
    static std::array<BandFilter, BandCount - 1> MakeFilters(
        const CrossoverFrequencies &crossoverFrequencies, std::index_sequence<Bands...>)
    {
        return {BandFilter(crossoverFrequencies[Bands])...};
    }

    std::array<BandFilter, BandCount - 1> mFilters;
};

/** Convenience alias for a four-band splitter. */
template <typename BandFilter>
using FourBandSplitter = MultiBandSplitter<4, BandFilter>;
