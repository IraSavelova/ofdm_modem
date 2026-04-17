#include "encode_engine.hh"
#include "common.hh"
#include "utils.hh"
#include "polar_encoder.hh"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>

template <typename value, typename cmplx, int rate>
struct Encoder : public Common
{
    typedef int8_t code_type;
    static const int guard_len = rate / 300;
    static const int symbol_len = guard_len * 40;

    DSP::WritePCM<value> *pcm;
    DSP::FastFourierTransform<symbol_len, cmplx, -1> fwd;
    DSP::FastFourierTransform<symbol_len, cmplx, 1> bwd;
    CODE::PolarEncoder<code_type> polar_encoder;

    code_type code[bits_max], perm[bits_max], mesg[bits_max], meta[data_tones];
    cmplx fdom[symbol_len];
    cmplx tdom[symbol_len];
    cmplx test[symbol_len];
    cmplx kern[symbol_len];
    cmplx guard[guard_len];
    cmplx tone[tone_count];
    cmplx temp[tone_count];
    value weight[guard_len];
    value papr[symbols_max];

    // Один scrambler на весь поток пакетов
    CODE::Xorshift32 scrambler;

    static int bin(int carrier)
    {
        return (carrier + symbol_len) % symbol_len;
    }

    static int nrz(bool bit)
    {
        return 1 - 2 * bit;
    }

    int header_bytes() const
    {
        return 2; // payload length
    }

    int max_payload_bytes() const
    {
        return data_bytes - header_bytes();
    }

    void clipping_and_filtering(value scale)
    {
        for (int i = 0; i < symbol_len; ++i)
        {
            value pwr = norm(tdom[i]);
            if (pwr > value(1))
                tdom[i] /= sqrt(pwr);
        }

        fwd(fdom, tdom);

        for (int i = 0; i < symbol_len; ++i)
        {
            int j = bin(i + tone_off);
            if (i >= tone_count)
                fdom[j] = 0;
            else
                fdom[j] *= value(1) / (scale * symbol_len);
        }

        bwd(tdom, fdom);

        for (int i = 0; i < symbol_len; ++i)
            tdom[i] *= scale;

        auto clamp = [](value v)
        {
            return v < value(-1) ? value(-1)
                 : v > value(1)  ? value(1)
                                  : v;
        };

        for (int i = 0; i < symbol_len; ++i)
            tdom[i] = cmplx(clamp(tdom[i].real()), clamp(tdom[i].imag()));
    }

    void symbol(int symbol_number)
    {
        value scale = value(0.5) / std::sqrt(value(tone_count));

        if (symbol_number < 0)
        {
            for (int i = 0; i < symbol_len; ++i)
                fdom[i] = 0;

            for (int i = 0; i < tone_count; ++i)
                fdom[bin(i + tone_off)] = tone[i];

            bwd(tdom, fdom);

            for (int i = 0; i < symbol_len; ++i)
                tdom[i] *= scale;
        }
        else
        {
            value best_papr = 1000;

            for (int seed_value = 0; seed_value < 128; ++seed_value)
            {
                for (int i = 0; i < tone_count; ++i)
                    temp[i] = tone[i];

                hadamard_encoder(seed, seed_value);

                for (int i = 0; i < seed_tones; ++i)
                    temp[i * block_length + seed_off] *= seed[i];

                if (seed_value)
                {
                    CODE::MLS seq(mls2_poly, seed_value);
                    for (int i = 0; i < tone_count; ++i)
                        if (i % block_length != seed_off)
                            temp[i] *= nrz(seq());
                }

                for (int i = 0; i < symbol_len; ++i)
                    fdom[i] = 0;

                for (int i = 0; i < tone_count; ++i)
                    fdom[bin(i + tone_off)] = temp[i];

                bwd(test, fdom);

                for (int i = 0; i < symbol_len; ++i)
                    test[i] *= scale;

                value peak = 0;
                value mean = 0;

                for (int i = 0; i < symbol_len; ++i)
                {
                    value power(norm(test[i]));
                    peak = std::max(peak, power);
                    mean += power;
                }

                mean /= symbol_len;
                value test_papr(peak / mean);

                if (test_papr < best_papr)
                {
                    best_papr = test_papr;
                    papr[symbol_number] = test_papr;

                    for (int i = 0; i < symbol_len; ++i)
                        tdom[i] = test[i];

                    if (test_papr < 5)
                        break;
                }
            }
			//std::cerr << "WRITE SYMBOL\n";
        }

        clipping_and_filtering(scale);

        if (symbol_number != -1)
        {
            for (int i = 0; i < guard_len; ++i)
                guard[i] = DSP::lerp(guard[i], tdom[i + symbol_len - guard_len], weight[i]);

            pcm->write(reinterpret_cast<value *>(guard), guard_len, 2);
        }

        for (int i = 0; i < guard_len; ++i)
            guard[i] = tdom[i];

        pcm->write(reinterpret_cast<value *>(tdom), symbol_len, 2);
    }

    void finish()
    {
        for (int i = 0; i < guard_len; ++i)
            guard[i] *= value(1) - weight[i];

        pcm->write(reinterpret_cast<value *>(guard), guard_len, 2);

        for (int i = 0; i < guard_len; ++i)
            guard[i] = 0;
    }

    void leading_noise(int num = 1)
    {
        CODE::MLS noise(mls2_poly);
        for (int j = 0; j < num; ++j)
        {
            for (int i = 0; i < tone_count; ++i)
                tone[i] = nrz(noise());

            symbol(-3);
        }
    }

    void schmidl_cox()
    {
        CODE::MLS seq0(mls0_poly, mls0_seed);
        for (int i = 0; i < tone_count; ++i)
            tone[i] = nrz(seq0());

        symbol(-2);
        symbol(-1);
    }

    void meta_data(uint64_t md)
    {
        for (int i = 0; i < 56; ++i)
            mesg[i] = nrz((md >> i) & 1);

        crc0.reset();
        crc0(md << 8);

        for (int i = 0; i < 16; ++i)
            mesg[i + 56] = nrz((crc0() >> i) & 1);

        polar_encoder(code, mesg, frozen_256_72, 8);
        shuffle(meta, code, 8);
    }

    cmplx map_bits(code_type *b, int bits)
    {
        switch (bits)
        {
        case 1:  return PhaseShiftKeying<2, cmplx, code_type>::map(b);
        case 2:  return PhaseShiftKeying<4, cmplx, code_type>::map(b);
        case 3:  return PhaseShiftKeying<8, cmplx, code_type>::map(b);
        case 4:  return QuadratureAmplitudeModulation<16, cmplx, code_type>::map(b);
        case 6:  return QuadratureAmplitudeModulation<64, cmplx, code_type>::map(b);
        case 8:  return QuadratureAmplitudeModulation<256, cmplx, code_type>::map(b);
        case 10: return QuadratureAmplitudeModulation<1024, cmplx, code_type>::map(b);
        case 12: return QuadratureAmplitudeModulation<4096, cmplx, code_type>::map(b);
        }
        return 0;
    }

    value mod_distance()
    {
        switch (mod_bits)
        {
        case 1:  return PhaseShiftKeying<2, cmplx, code_type>::DIST;
        case 2:  return PhaseShiftKeying<4, cmplx, code_type>::DIST;
        case 3:  return PhaseShiftKeying<8, cmplx, code_type>::DIST;
        case 4:  return QuadratureAmplitudeModulation<16, cmplx, code_type>::DIST;
        case 6:  return QuadratureAmplitudeModulation<64, cmplx, code_type>::DIST;
        case 8:  return QuadratureAmplitudeModulation<256, cmplx, code_type>::DIST;
        case 10: return QuadratureAmplitudeModulation<1024, cmplx, code_type>::DIST;
        case 12: return QuadratureAmplitudeModulation<4096, cmplx, code_type>::DIST;
        }
        return 2;
    }

    void shuffle(code_type *dest, const code_type *src, int order)
    {
        if (order == 8)
        {
            CODE::XorShiftMask<int, 8, 1, 1, 2, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 256; ++i) dest[i] = src[seq()];
        }
        else if (order == 11)
        {
            CODE::XorShiftMask<int, 11, 1, 3, 4, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 2048; ++i) dest[i] = src[seq()];
        }
        else if (order == 12)
        {
            CODE::XorShiftMask<int, 12, 1, 1, 4, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 4096; ++i) dest[i] = src[seq()];
        }
        else if (order == 13)
        {
            CODE::XorShiftMask<int, 13, 1, 1, 9, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 8192; ++i) dest[i] = src[seq()];
        }
        else if (order == 14)
        {
            CODE::XorShiftMask<int, 14, 1, 5, 10, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 16384; ++i) dest[i] = src[seq()];
        }
        else if (order == 15)
        {
            CODE::XorShiftMask<int, 15, 1, 1, 3, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 32768; ++i) dest[i] = src[seq()];
        }
        else if (order == 16)
        {
            CODE::XorShiftMask<int, 16, 1, 1, 14, 1> seq;
            dest[0] = src[0];
            for (int i = 1; i < 65536; ++i) dest[i] = src[seq()];
        }
    }

    void guard_interval_weights()
    {
        for (int i = 0; i < guard_len / 4; ++i)
            weight[i] = 0;

        for (int i = guard_len / 4; i < guard_len / 4 + guard_len / 2; ++i)
        {
            value x = value(i - guard_len / 4) / value(guard_len / 2 - 1);
            weight[i] = value(0.5) * (value(1) - std::cos(DSP::Const<value>::Pi() * x));
        }

        for (int i = guard_len / 4 + guard_len / 2; i < guard_len; ++i)
            weight[i] = 1;
    }

    void encode_frame(const uint8_t *payload, int payload_size)
    {
		std::cerr << "FRAME SYMBOL\n";
        const int hdr = header_bytes();
        const int max_payload = max_payload_bytes();

        if (payload_size < 0)
            payload_size = 0;
        if (payload_size > max_payload)
            payload_size = max_payload;

        // Собираем data[] = [len_lo][len_hi][payload...][padding]
        for (int i = 0; i < data_bytes; ++i)
            data[i] = 0;

        data[0] = static_cast<uint8_t>(payload_size & 0xFF);
        data[1] = static_cast<uint8_t>((payload_size >> 8) & 0xFF);

        for (int i = 0; i < payload_size; ++i)
            data[hdr + i] = payload[i];

        // scrambling всего блока
        for (int i = 0; i < data_bytes; ++i)
            data[i] ^= scrambler();

        // каждый кусок = отдельный пакет
        schmidl_cox();

        for (int i = 0; i < data_bits; ++i)
            mesg[i] = nrz(CODE::get_le_bit(data, i));

        crc1.reset();
        for (int i = 0; i < data_bytes; ++i)
            crc1(data[i]);

        for (int i = 0; i < 32; ++i)
            mesg[i + data_bits] = nrz((crc1() >> i) & 1);

        polar_encoder(code, mesg, frozen_bits, code_order);
        shuffle(perm, code, code_order);

        CODE::MLS seq1(mls1_poly);

        for (int j = 0, k = 0, m = 0; j < symbol_count + 1; ++j)
        {
            seed_off = (block_skew * j + first_seed) % block_length;

            for (int i = 0; i < tone_count; ++i)
            {
                if (i % block_length == seed_off)
                {
                    tone[i] = nrz(seq1());
                }
                else if (j)
                {
                    int bits = mod_bits;
                    if (mod_bits == 3 && k % 32 == 30)   bits = 2;
                    if (mod_bits == 6 && k % 64 == 60)   bits = 4;
                    if (mod_bits == 10 && k % 128 == 120) bits = 8;
                    if (mod_bits == 12 && k % 128 == 120) bits = 8;

                    tone[i] = map_bits(perm + k, bits);
                    k += bits;
                }
                else
                {
                    tone[i] = map_bits(meta + m++, 1);
                }
            }

            symbol(j);
        }

        DSP::quick_sort(papr, symbol_count + 1);
        // std::cerr << "PAPR (dB): "
        //           << DSP::decibel(papr[0]) << " .. "
        //           << DSP::decibel(papr[symbol_count / 2]) << " .. "
        //           << DSP::decibel(papr[symbol_count]) << std::endl;
    }

    Encoder(DSP::WritePCM<value> *pcm, int freq_off, int64_t call_sign, int oper_mode)
        : pcm(pcm)
    {
        if (!setup(oper_mode))
            return;

        int offset = (freq_off * symbol_len) / rate;
        tone_off = offset - tone_count / 2;

        guard_interval_weights();
        meta_data((call_sign << 8) | oper_mode);
        leading_noise();
    }
};

int64_t base40_encoder(const char *str)
{
    int64_t acc = 0;
    for (char c = *str++; c; c = *str++)
    {
        acc *= 40;
        if (c == '/')
            acc += 3;
        else if (c >= '0' && c <= '9')
            acc += c - '0' + 4;
        else if (c >= 'a' && c <= 'z')
            acc += c - 'a' + 14;
        else if (c >= 'A' && c <= 'Z')
            acc += c - 'A' + 14;
        else if (c != ' ')
            return -1;
    }
    return acc;
}

static bool build_oper_mode(const EncodeConfig &cfg, int &oper_mode, std::string &error)
{
    oper_mode = 0;

    const std::string &modulation = cfg.modulation;
    if (modulation == "BPSK") oper_mode |= 0 << 4;
    else if (modulation == "QPSK") oper_mode |= 1 << 4;
    else if (modulation == "8PSK") oper_mode |= 2 << 4;
    else if (modulation == "QAM16") oper_mode |= 3 << 4;
    else if (modulation == "QAM64") oper_mode |= 4 << 4;
    else if (modulation == "QAM256") oper_mode |= 5 << 4;
    else if (modulation == "QAM1024") oper_mode |= 6 << 4;
    else if (modulation == "QAM4096") oper_mode |= 7 << 4;
    else
    {
        error = "Unsupported modulation.";
        return false;
    }

    const std::string &code_rate = cfg.code_rate;
    if (code_rate == "1/2") oper_mode |= 0 << 1;
    else if (code_rate == "2/3") oper_mode |= 1 << 1;
    else if (code_rate == "3/4") oper_mode |= 2 << 1;
    else if (code_rate == "5/6") oper_mode |= 3 << 1;
    else
    {
        error = "Unsupported code rate.";
        return false;
    }

    const std::string &frame_size = cfg.frame_size;
    if (frame_size == "short") oper_mode |= 0;
    else if (frame_size == "normal") oper_mode |= 1;
    else
    {
        error = "Unsupported frame size.";
        return false;
    }

    return true;
}

template <typename value, typename cmplx, int rate>
static EncodeResult encode_to_wav_typed(
    DSP::WriteWAV<value> &output_file,
    const EncodeConfig &cfg,
    int64_t call_sign,
    int oper_mode)
{
    EncodeResult result{false, ""};

    Encoder<value, cmplx, rate> enc(
        &output_file,
        cfg.center_freq_hz,
        call_sign,
        oper_mode
    );

    const int payload_bytes = enc.max_payload_bytes();
    if (payload_bytes <= 0)
    {
        result.error = "Invalid payload size.";
        return result;
    }

    std::vector<uint8_t> buffer(payload_bytes);

    for (const auto &file : cfg.input_files)
    {
        std::ifstream f(file, std::ios::binary);
        if (!f.is_open())
        {
            result.error = "Couldn't open file \"" + file + "\" for reading.";
            return result;
        }

        while (true)
        {
            f.read(reinterpret_cast<char *>(buffer.data()), payload_bytes);
            const int got = static_cast<int>(f.gcount());

            if (got <= 0)
                break;

            enc.encode_frame(buffer.data(), got);

            if (got < payload_bytes)
                break;
        }
    }

    enc.finish();
    result.ok = true;
    return result;
}

EncodeResult encode_to_wav(const EncodeConfig &cfg)
{
    EncodeResult result{false, ""};

    if (cfg.input_files.empty())
    {
        result.error = "No input files provided.";
        return result;
    }

    if (cfg.sample_rate != 44100 && cfg.sample_rate != 48000)
    {
        result.error = "Unsupported sample rate.";
        return result;
    }

    if (cfg.center_freq_hz % 300)
    {
        result.error = "Frequency offset must be divisible by 300.";
        return result;
    }

    int64_t call_sign = base40_encoder(cfg.callsign.c_str());
    if (call_sign <= 0 || call_sign >= 262144000000000L)
    {
        result.error = "Unsupported call sign.";
        return result;
    }

    int oper_mode = 0;
    if (!build_oper_mode(cfg, oper_mode, result.error))
        return result;

    int band_width = 2400;
    if ((cfg.channels == 1 && cfg.center_freq_hz < band_width / 2) ||
        cfg.center_freq_hz < band_width / 2 - cfg.sample_rate / 2 ||
        cfg.center_freq_hz > cfg.sample_rate / 2 - band_width / 2)
    {
        result.error = "Unsupported frequency offset.";
        return result;
    }

    std::cerr << std::fixed << std::setprecision(1);

    typedef float value;
    typedef DSP::Complex<value> cmplx;

    DSP::WriteWAV<value> output_file(
        cfg.output_path.c_str(),
        cfg.sample_rate,
        cfg.bits,
        cfg.channels
    );

    output_file.silence(cfg.sample_rate);

    switch (cfg.sample_rate)
    {
    case 44100:
        result = encode_to_wav_typed<value, cmplx, 44100>(
            output_file, cfg, call_sign, oper_mode
        );
        break;

    case 48000:
        result = encode_to_wav_typed<value, cmplx, 48000>(
            output_file, cfg, call_sign, oper_mode
        );
        break;

    default:
        result.error = "Unsupported sample rate.";
        return result;
    }

    if (!result.ok)
        return result;

    output_file.silence(cfg.sample_rate);
    return result;
}