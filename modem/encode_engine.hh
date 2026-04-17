#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct EncodeConfig
{
    std::string output_path;
    int sample_rate = 48000;
    int bits = 16;
    int channels = 2;
    int center_freq_hz = 1200;
    std::string callsign = "TEST";
    std::string modulation = "QPSK";
    std::string code_rate = "1/2";
    std::string frame_size = "normal";
    std::vector<std::string> input_files;
};

struct EncodeResult
{
    bool ok = false;
    std::string error;
};

struct EncodeChunkResult
{
    bool ok = false;
    std::string error;
    std::vector<float> pcm;
};

struct EncoderSessionHandle
{
    void* impl = nullptr;
    int sample_rate = 0;
};

EncodeResult encode_to_wav(const EncodeConfig& cfg);
int max_payload_bytes_for_config(const EncodeConfig& cfg);
EncoderSessionHandle create_encoder_session(const EncodeConfig& cfg);
void destroy_encoder_session(EncoderSessionHandle& h);
EncodeChunkResult encode_chunk_to_pcm(EncoderSessionHandle& h, const std::vector<uint8_t>& payload);