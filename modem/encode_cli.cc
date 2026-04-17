#include "encode_engine.hh"

#include <cstdlib>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 11)
    {
        std::cerr << "usage: " << argv[0]
                  << " OUTPUT RATE BITS CHANNELS OFFSET CALLSIGN MODULATION CODERATE FRAMESIZE INPUT.."
                  << std::endl;
        return 1;
    }

    EncodeConfig cfg;
    cfg.output_path = argv[1];
    cfg.sample_rate = std::atoi(argv[2]);
    cfg.bits = std::atoi(argv[3]);
    cfg.channels = std::atoi(argv[4]);
    cfg.center_freq_hz = std::atoi(argv[5]);
    cfg.callsign = argv[6];
    cfg.modulation = argv[7];
    cfg.code_rate = argv[8];
    cfg.frame_size = argv[9];

    for (int i = 10; i < argc; ++i)
        cfg.input_files.push_back(argv[i]);

    auto result = encode_to_wav(cfg);
    if (!result.ok)
    {
        std::cerr << result.error << std::endl;
        return 1;
    }

    return 0;
}