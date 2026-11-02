#pragma once

#if defined(__GNUC__) || defined(__clang__)
/** Four-lane SIMD storage used for two-channel audio samples. */
using SimdFloat4 = float __attribute__((vector_size(16)));

/** Packs two channel samples into the first two lanes of a SIMD value. */
inline SimdFloat4 MakeSimdFloat4(float first, float second)
{
    SimdFloat4 value{first, second, 0.0f, 0.0f};
    return value;
}

/** A stereo frame stored in SIMD lanes, with left and right in lanes 0 and 1. */
struct StereoFrame {
    SimdFloat4 mSamples;

    /** Constructs a frame from its left and right channel samples. */
    static StereoFrame FromChannels(float left, float right)
    {
        return {MakeSimdFloat4(left, right)};
    }

    /** Returns the left-channel sample. */
    float Left() const { return mSamples[0]; }

    /** Returns the right-channel sample. */
    float Right() const { return mSamples[1]; }

    /** Replaces the left and right channel samples. */
    void SetChannels(float left, float right)
    {
        mSamples[0] = left;
        mSamples[1] = right;
    }
};
#endif