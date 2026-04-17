#pragma once

#include <string>
#include <vector>

struct DecodeConfig
{
    int sample_rate = 48000;
    int channels = 1;
    int center_freq_hz = 0;
    std::string output_path;
};

struct DecodeChunkResult
{
    bool ok = false;
    std::string error;
    int packets_written = 0;
};

struct DecoderSessionHandle
{
    void* impl = nullptr;
    int sample_rate = 0;
};

DecoderSessionHandle create_decoder_session(const DecodeConfig& cfg);
void destroy_decoder_session(DecoderSessionHandle& h);
DecodeChunkResult decode_chunk_to_file(DecoderSessionHandle& h, const std::vector<float>& pcm);