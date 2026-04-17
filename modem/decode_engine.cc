#include "decode_engine.hh"
#include "common.hh"
#include "schmidl_cox.hh"
#include "bip_buffer.hh"
#include "theil_sen.hh"
#include "blockdc.hh"
#include "hilbert.hh"
#include "phasor.hh"
#include "delay.hh"
#include "polar_list_decoder.hh"
#include "hadamard_decoder.hh"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct SessionStats
    {
        std::atomic<bool> fatal_error{false};
        std::atomic<int> packets_written{0};
        std::mutex error_mutex;
        std::string error;

        void set_error(const std::string &e)
        {
            {
                std::lock_guard<std::mutex> lk(error_mutex);
                error = e;
            }
            fatal_error.store(true, std::memory_order_release);
        }

        std::string get_error() const
        {
            std::lock_guard<std::mutex> lk(const_cast<std::mutex &>(error_mutex));
            return error;
        }
    };

    template <typename T>
    struct QueuePCMReader : public DSP::ReadPCM<T>
    {
        std::deque<T> buffer_;
        std::mutex mutex_;
        std::condition_variable cv_;
        int sample_rate_;
        int channels_;
        bool closed_;
        bool ok_;

        QueuePCMReader(int sample_rate, int channels)
            : sample_rate_(sample_rate), channels_(channels), closed_(false), ok_(true)
        {
        }

        bool good() override
        {
            std::lock_guard<std::mutex> lk(mutex_);
            return ok_;
        }

        int rate() override
        {
            return sample_rate_;
        }

        int channels() override
        {
            return channels_;
        }

        void append(const T *data, size_t samples)
        {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (closed_ || !ok_)
                    return;

                buffer_.insert(buffer_.end(), data, data + samples);
            }
            cv_.notify_all();
        }

        void close()
        {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                closed_ = true;
            }
            cv_.notify_all();
        }

        void read(T *data, int count, int channels = -1) override
        {
            const int ch = (channels == -1) ? channels_ : channels;

            if (ch != channels_)
            {
                std::fill(data, data + static_cast<size_t>(count) * static_cast<size_t>(ch), T(0));
                std::lock_guard<std::mutex> lk(mutex_);
                ok_ = false;
                return;
            }

            const size_t need = static_cast<size_t>(count) * static_cast<size_t>(channels_);

            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&]
                     { return buffer_.size() >= need || closed_ || !ok_; });

            if (buffer_.size() < need)
            {
                std::fill(data, data + need, T(0));
                ok_ = false;
                return;
            }

            for (size_t i = 0; i < need; ++i)
            {
                data[i] = buffer_.front();
                buffer_.pop_front();
            }
        }

        void skip(int count) override
        {
            const size_t need = static_cast<size_t>(count) * static_cast<size_t>(channels_);

            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&]
                     { return buffer_.size() >= need || closed_ || !ok_; });

            if (buffer_.size() < need)
            {
                ok_ = false;
                return;
            }

            for (size_t i = 0; i < need; ++i)
                buffer_.pop_front();
        }
    };

    static int64_t base40_encoder(const char *str)
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

    template <typename value, typename cmplx, int rate>
    struct Decoder : Common
    {
        typedef int16_t code_type;
        typedef SIMD<code_type, 32> mesg_type;
        typedef DSP::Const<value> Const;

        static const int guard_len = rate / 300;
        static const int symbol_len = guard_len * 40;
        static const int filter_len = 129;
        static const int extended_len = symbol_len + guard_len;
        static const int buffer_len = 5 * extended_len;
        static const int search_pos = extended_len;
        static const int tone_off = -tone_count / 2;

        DSP::ReadPCM<value> *pcm;
        SessionStats *stats;
        std::ofstream output_file;

        DSP::FastFourierTransform<symbol_len, cmplx, -1> fwd;
        DSP::BlockDC<value, value> blockdc;
        DSP::Hilbert<cmplx, filter_len> hilbert;
        DSP::BipBuffer<cmplx, buffer_len> input_hist;
        DSP::TheilSenEstimator<value, tone_count> tse;
        SchmidlCox<value, cmplx, search_pos, symbol_len, guard_len> correlator;
        CODE::HadamardDecoder<7> hadamard_decoder;
        CODE::PolarListDecoder<mesg_type, code_max> polar_decoder;

        mesg_type mesg[bits_max];
        code_type code[bits_max], perm[bits_max];
        cmplx demod[tone_count], chan[tone_count], tone[tone_count];
        cmplx fdom[symbol_len], tdom[symbol_len];
        value index[tone_count], phase[tone_count];
        value snr[symbols_max];
        value cfo_rad, sfo_rad;
        int symbol_pos;
        int crc_bits;

        // scrambler должен жить всю RX-сессию, как и в encoder
        CODE::Xorshift32 scrambler;

        static int bin(int carrier)
        {
            return (carrier + symbol_len) % symbol_len;
        }

        static value nrz(bool bit)
        {
            return 1 - 2 * bit;
        }

        static cmplx demod_or_erase(cmplx curr, cmplx prev)
        {
            if (norm(prev) > 0)
            {
                cmplx demod = curr / prev;
                if (norm(demod) < 4)
                    return demod;
            }
            return 0;
        }

        static void base40_decoder(char *str, int64_t val, int len)
        {
            for (int i = len - 1; i >= 0; --i, val /= 40)
                str[i] = "   /0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[val % 40];
        }

        const cmplx *mls0_seq()
        {
            CODE::MLS seq0(mls0_poly, mls0_seed);
            value cur = 0, prv = 0;
            for (int i = 0; i < tone_count; ++i, prv = cur)
                fdom[bin(i + tone_off)] = prv * (cur = nrz(seq0()));
            return fdom;
        }

        cmplx map_bits(code_type *b, int bits)
        {
            switch (bits)
            {
            case 1:
                return PhaseShiftKeying<2, cmplx, code_type>::map(b);
            case 2:
                return PhaseShiftKeying<4, cmplx, code_type>::map(b);
            case 3:
                return PhaseShiftKeying<8, cmplx, code_type>::map(b);
            case 4:
                return QuadratureAmplitudeModulation<16, cmplx, code_type>::map(b);
            case 6:
                return QuadratureAmplitudeModulation<64, cmplx, code_type>::map(b);
            case 8:
                return QuadratureAmplitudeModulation<256, cmplx, code_type>::map(b);
            case 10:
                return QuadratureAmplitudeModulation<1024, cmplx, code_type>::map(b);
            case 12:
                return QuadratureAmplitudeModulation<4096, cmplx, code_type>::map(b);
            }
            return 0;
        }

        void demap_soft(code_type *b, cmplx c, value precision, int bits)
        {
            switch (bits)
            {
            case 1:
                return PhaseShiftKeying<2, cmplx, code_type>::soft(b, c, precision);
            case 2:
                return PhaseShiftKeying<4, cmplx, code_type>::soft(b, c, precision);
            case 3:
                return PhaseShiftKeying<8, cmplx, code_type>::soft(b, c, precision);
            case 4:
                return QuadratureAmplitudeModulation<16, cmplx, code_type>::soft(b, c, precision);
            case 6:
                return QuadratureAmplitudeModulation<64, cmplx, code_type>::soft(b, c, precision);
            case 8:
                return QuadratureAmplitudeModulation<256, cmplx, code_type>::soft(b, c, precision);
            case 10:
                return QuadratureAmplitudeModulation<1024, cmplx, code_type>::soft(b, c, precision);
            case 12:
                return QuadratureAmplitudeModulation<4096, cmplx, code_type>::soft(b, c, precision);
            }
        }

        void demap_hard(code_type *b, cmplx c, int bits)
        {
            switch (bits)
            {
            case 1:
                return PhaseShiftKeying<2, cmplx, code_type>::hard(b, c);
            case 2:
                return PhaseShiftKeying<4, cmplx, code_type>::hard(b, c);
            case 3:
                return PhaseShiftKeying<8, cmplx, code_type>::hard(b, c);
            case 4:
                return QuadratureAmplitudeModulation<16, cmplx, code_type>::hard(b, c);
            case 6:
                return QuadratureAmplitudeModulation<64, cmplx, code_type>::hard(b, c);
            case 8:
                return QuadratureAmplitudeModulation<256, cmplx, code_type>::hard(b, c);
            case 10:
                return QuadratureAmplitudeModulation<1024, cmplx, code_type>::hard(b, c);
            case 12:
                return QuadratureAmplitudeModulation<4096, cmplx, code_type>::hard(b, c);
            }
        }

        void shuffle(code_type *dest, const code_type *src, int order)
        {
            if (order == 8)
            {
                CODE::XorShiftMask<int, 8, 1, 1, 2, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 256; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 11)
            {
                CODE::XorShiftMask<int, 11, 1, 3, 4, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 2048; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 12)
            {
                CODE::XorShiftMask<int, 12, 1, 1, 4, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 4096; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 13)
            {
                CODE::XorShiftMask<int, 13, 1, 1, 9, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 8192; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 14)
            {
                CODE::XorShiftMask<int, 14, 1, 5, 10, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 16384; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 15)
            {
                CODE::XorShiftMask<int, 15, 1, 1, 3, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 32768; ++i)
                    dest[seq()] = src[i];
            }
            else if (order == 16)
            {
                CODE::XorShiftMask<int, 16, 1, 1, 14, 1> seq;
                dest[0] = src[0];
                for (int i = 1; i < 65536; ++i)
                    dest[seq()] = src[i];
            }
        }

        const cmplx *next_sample()
        {
            cmplx tmp;
            pcm->read(reinterpret_cast<value *>(&tmp), 1);
            if (pcm->channels() == 1)
                tmp = hilbert(blockdc(tmp.real()));
            return input_hist(tmp);
        }

        int64_t meta_data()
        {
            shuffle(code, perm, 8);
            polar_decoder(nullptr, mesg, code, frozen_256_72, 8);

            int best = -1;
            for (int k = 0; k < mesg_type::SIZE; ++k)
            {
                crc0.reset();
                for (int i = 0; i < 72; ++i)
                    crc0(mesg[i].v[k] < 0);
                if (crc0() == 0)
                {
                    best = k;
                    break;
                }
            }

            if (best < 0)
                return -1;

            uint64_t md = 0;
            for (int i = 0; i < 56; ++i)
                md |= uint64_t(mesg[i].v[best] < 0) << i;

            return md;
        }

        void write_payload_to_file()
        {
            const int hdr = 2;

            for (int i = 0; i < data_bytes; ++i)
                data[i] ^= scrambler();

            uint16_t payload_size =
                static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8);

            if (payload_size > data_bytes - hdr)
            {
                std::cerr << "payload length header damaged." << std::endl;
                return;
            }

            output_file.write(reinterpret_cast<const char *>(data + hdr), payload_size);
            output_file.flush();

            stats->packets_written.fetch_add(1, std::memory_order_acq_rel);
        }

        void run()
        {
            blockdc.samples(filter_len);
            DSP::Phasor<cmplx> osc;
            const cmplx *buf;
            int sample_count = 0;

            while (true)
            {
                do
                {
                    if (!pcm->good())
                        return;
                    buf = next_sample();
                    ++sample_count;
                } while (!correlator(buf));

                symbol_pos = correlator.symbol_pos;
                cfo_rad = correlator.cfo_rad;

                std::cerr << "symbol pos: " << sample_count - buffer_len + symbol_pos << std::endl;
                std::cerr << "coarse cfo: " << cfo_rad * (rate / Const::TwoPi()) << " Hz" << std::endl;

                osc.omega(-cfo_rad);

                for (int i = 0; i < symbol_len; ++i)
                    tdom[i] = buf[i + symbol_pos] * osc();
                fwd(fdom, tdom);
                for (int i = 0; i < tone_count; ++i)
                    tone[i] = fdom[bin(i + tone_off)];

                for (int i = 0; i < symbol_len; ++i)
                    tdom[i] = buf[i + symbol_pos + symbol_len] * osc();
                for (int i = 0; i < guard_len; ++i)
                    osc();
                fwd(fdom, tdom);
                for (int i = 0; i < tone_count; ++i)
                    chan[i] = fdom[bin(i + tone_off)];

                for (int i = 0; i < tone_count; ++i)
                {
                    index[i] = tone_off + i;
                    phase[i] = arg(demod_or_erase(chan[i], tone[i]));
                }

                tse.compute(index, phase, tone_count);

                std::cerr << "coarse sfo: " << -1000000 * tse.slope() / Const::TwoPi() << " ppm" << std::endl;
                std::cerr << "residual cfo: "
                          << 1000 * tse.yint() * rate / (Const::TwoPi() * symbol_len)
                          << " mHz" << std::endl;

                for (int i = 0; i < tone_count; ++i)
                    tone[i] *= DSP::polar<value>(1, tse(i + tone_off));
                for (int i = 0; i < tone_count; ++i)
                    chan[i] = DSP::lerp(chan[i], tone[i], value(0.5));

                CODE::MLS seq0(mls0_poly, mls0_seed);
                for (int i = 0; i < tone_count; ++i)
                    chan[i] *= nrz(seq0());

                for (int i = 0; i < symbol_len; ++i)
                    tdom[i] = buf[i + symbol_pos + symbol_len + extended_len] * osc();
                for (int i = 0; i < guard_len; ++i)
                    osc();
                fwd(fdom, tdom);

                CODE::MLS seq1(mls1_poly);
                auto clamp = [](int v)
                { return v < -127 ? -127 : v > 127 ? 127
                                                   : v; };

                mod_bits = 1;
                oper_mode = -1;
                symbol_count = 0;

                for (int j = 0, k = 0; j < symbol_count + 1; ++j)
                {
                    seed_off = (block_skew * j + first_seed) % block_length;

                    if (j)
                    {
                        for (int i = 0; i < extended_len; ++i)
                            correlator(buf = next_sample());

                        for (int i = 0; i < symbol_len; ++i)
                            tdom[i] = buf[i] * osc();
                        for (int i = 0; i < guard_len; ++i)
                            osc();
                        fwd(fdom, tdom);
                    }

                    for (int i = 0; i < tone_count; ++i)
                        tone[i] = fdom[bin(i + tone_off)];

                    for (int i = seed_off; i < tone_count; i += block_length)
                        tone[i] *= nrz(seq1());

                    for (int i = 0; i < tone_count; ++i)
                        demod[i] = demod_or_erase(tone[i], chan[i]);

                    for (int i = 0; i < seed_tones; ++i)
                        seed[i] = clamp(std::nearbyint(127 * demod[i * block_length + seed_off].real()));

                    int seed_value = hadamard_decoder(seed);
                    if (seed_value < 0)
                    {
                        std::cerr << "seed value damaged" << std::endl;
                        oper_mode = -1;
                        break;
                    }

                    hadamard_encoder(seed, seed_value);
                    for (int i = 0; i < seed_tones; ++i)
                    {
                        tone[block_length * i + seed_off] *= seed[i];
                        demod[block_length * i + seed_off] *= seed[i];
                    }

                    for (int i = 0; i < seed_tones; ++i)
                    {
                        index[i] = tone_off + block_length * i + seed_off;
                        phase[i] = arg(demod[block_length * i + seed_off]);
                    }

                    tse.compute(index, phase, seed_tones);

                    for (int i = 0; i < tone_count; ++i)
                        demod[i] *= DSP::polar<value>(1, -tse(i + tone_off));
                    for (int i = 0; i < tone_count; ++i)
                        chan[i] *= DSP::polar<value>(1, tse(i + tone_off));

                    if (seed_value)
                    {
                        CODE::MLS seq(mls2_poly, seed_value);
                        for (int i = 0; i < tone_count; ++i)
                            if (i % block_length != seed_off)
                                demod[i] *= nrz(seq());
                    }

                    value sp = 0;
                    value np = 0;

                    for (int i = 0, l = k; i < tone_count; ++i)
                    {
                        cmplx hard(1, 0);

                        if (i % block_length != seed_off)
                        {
                            int bits = mod_bits;
                            if (mod_bits == 3 && l % 32 == 30)
                                bits = 2;
                            if (mod_bits == 6 && l % 64 == 60)
                                bits = 4;
                            if (mod_bits == 10 && l % 128 == 120)
                                bits = 8;
                            if (mod_bits == 12 && l % 128 == 120)
                                bits = 8;

                            demap_hard(perm + l, demod[i], bits);
                            hard = map_bits(perm + l, bits);
                            l += bits;
                        }

                        cmplx error = demod[i] - hard;
                        sp += norm(hard);
                        np += norm(error);
                    }

                    value precision = sp / np;
                    snr[j] = precision;
                    precision = std::min(precision, value(1023));

                    for (int i = 0; i < tone_count; ++i)
                    {
                        if (i % block_length != seed_off)
                        {
                            int bits = mod_bits;
                            if (mod_bits == 3 && k % 32 == 30)
                                bits = 2;
                            if (mod_bits == 6 && k % 64 == 60)
                                bits = 4;
                            if (mod_bits == 10 && k % 128 == 120)
                                bits = 8;
                            if (mod_bits == 12 && k % 128 == 120)
                                bits = 8;

                            demap_soft(perm + k, demod[i], precision, bits);
                            k += bits;
                        }
                    }

                    if (!j)
                    {
                        int64_t meta_info = meta_data();
                        if (meta_info < 0)
                        {
                            std::cerr << "preamble decoding error." << std::endl;
                            break;
                        }

                        int64_t call = meta_info >> 8;
                        if (call == 0 || call >= 262144000000000L)
                        {
                            std::cerr << "call sign unsupported." << std::endl;
                            break;
                        }

                        char call_sign[10];
                        base40_decoder(call_sign, call, 9);
                        call_sign[9] = 0;
                        std::cerr << "call sign: " << call_sign << std::endl;

                        int mode = meta_info & 255;
                        if (!setup(mode))
                            break;

                        k = 0;
                        for (int i = 0; i < symbol_pos + symbol_len + extended_len; ++i)
                            correlator(buf = next_sample());

                        std::cerr << "oper mode: " << oper_mode << std::endl;
                    }

                    for (int i = seed_off; i < tone_count; i += block_length)
                        chan[i] = DSP::lerp(chan[i], tone[i], value(0.5));
                }

                if (oper_mode < 0)
                    continue;

                DSP::quick_sort(snr, symbol_count + 1);
                std::cerr << "Es/N0 (dB): "
                          << DSP::decibel(snr[0]) << " .. "
                          << DSP::decibel(snr[symbol_count / 2]) << " .. "
                          << DSP::decibel(snr[symbol_count]) << std::endl;

                crc_bits = data_bits + 32;
                shuffle(code, perm, code_order);
                polar_decoder(nullptr, mesg, code, frozen_bits, code_order);

                int best = -1;
                for (int k = 0; k < mesg_type::SIZE; ++k)
                {
                    crc1.reset();
                    for (int i = 0; i < crc_bits; ++i)
                        crc1(mesg[i].v[k] < 0);
                    if (crc1() == 0)
                    {
                        best = k;
                        break;
                    }
                }

                if (best < 0)
                {
                    std::cerr << "payload decoding error." << std::endl;
                    continue;
                }

                for (int i = 0; i < data_bits; ++i)
                    CODE::set_le_bit(data, i, mesg[i].v[best] < 0);

                write_payload_to_file();
            }
        }

        Decoder(DSP::ReadPCM<value> *pcm, const std::string &output_path, SessionStats *stats)
            : pcm(pcm), stats(stats), output_file(output_path, std::ios::binary | std::ios::trunc), correlator(mls0_seq())
        {
            if (!output_file.is_open())
                throw std::runtime_error("Couldn't open file \"" + output_path + "\" for writing.");

            std::cerr << std::fixed << std::setprecision(1);
            run();
        }
    };

    template <typename value, typename cmplx, int rate>
    struct DecoderSessionImpl
    {
        QueuePCMReader<value> mem;
        SessionStats stats;
        std::thread worker;
        std::atomic<bool> started{false};

        DecoderSessionImpl(const DecodeConfig &cfg)
            : mem(cfg.sample_rate, cfg.channels)
        {
            worker = std::thread([this, cfg]()
                                 {
            try
            {
                Decoder<value, cmplx, rate>* dec =
                    new Decoder<value, cmplx, rate>(&mem, cfg.output_path, &stats);
                delete dec;
            }
            catch (const std::exception& e)
            {
                stats.set_error(e.what());
            }
            catch (...)
            {
                stats.set_error("Unknown decoder error.");
            } });

            started.store(true, std::memory_order_release);
        }

        ~DecoderSessionImpl()
        {
            mem.close();
            if (worker.joinable())
                worker.join();
        }

        int decode_chunk(const std::vector<value> &pcm)
        {
            if (stats.fatal_error.load(std::memory_order_acquire))
                throw std::runtime_error(stats.get_error());

            const int before = stats.packets_written.load(std::memory_order_acquire);
            mem.append(pcm.data(), pcm.size());
            const int after = stats.packets_written.load(std::memory_order_acquire);
            return after - before;
        }

        std::string current_error() const
        {
            return stats.get_error();
        }
    };

    static void validate_decode_config(const DecodeConfig &cfg)
    {
        if (cfg.sample_rate != 44100 && cfg.sample_rate != 48000)
            throw std::runtime_error("Unsupported sample rate.");

        if (cfg.channels != 1 && cfg.channels != 2)
            throw std::runtime_error("Only one or two channels supported.");

        if (cfg.output_path.empty())
            throw std::runtime_error("Output path is empty.");

        const int band_width = 2400;
        if ((cfg.channels == 1 && cfg.center_freq_hz < band_width / 2) ||
            cfg.center_freq_hz < band_width / 2 - cfg.sample_rate / 2 ||
            cfg.center_freq_hz > cfg.sample_rate / 2 - band_width / 2)
        {
            throw std::runtime_error("Unsupported frequency offset.");
        }
    }
} // namespace

DecoderSessionHandle create_decoder_session(const DecodeConfig &cfg)
{
    validate_decode_config(cfg);

    DecoderSessionHandle h{};
    h.impl = nullptr;
    h.sample_rate = cfg.sample_rate;

    using value = float;
    using cmplx = DSP::Complex<value>;

    if (cfg.sample_rate == 44100)
        h.impl = new DecoderSessionImpl<value, cmplx, 44100>(cfg);
    else
        h.impl = new DecoderSessionImpl<value, cmplx, 48000>(cfg);

    return h;
}

void destroy_decoder_session(DecoderSessionHandle &h)
{
    if (!h.impl)
        return;

    using value = float;
    using cmplx = DSP::Complex<value>;

    if (h.sample_rate == 44100)
        delete static_cast<DecoderSessionImpl<value, cmplx, 44100> *>(h.impl);
    else if (h.sample_rate == 48000)
        delete static_cast<DecoderSessionImpl<value, cmplx, 48000> *>(h.impl);

    h.impl = nullptr;
    h.sample_rate = 0;
}

DecodeChunkResult decode_chunk_to_file(DecoderSessionHandle &h, const std::vector<float> &pcm)
{
    DecodeChunkResult result{};
    result.ok = false;
    result.packets_written = 0;

    if (!h.impl)
    {
        result.error = "Decoder session is not initialized.";
        return result;
    }

    if (pcm.empty())
    {
        result.ok = true;
        return result;
    }

    using value = float;
    using cmplx = DSP::Complex<value>;

    try
    {
        if (h.sample_rate == 44100)
        {
            auto *dec = static_cast<DecoderSessionImpl<value, cmplx, 44100> *>(h.impl);
            result.packets_written = dec->decode_chunk(pcm);
        }
        else if (h.sample_rate == 48000)
        {
            auto *dec = static_cast<DecoderSessionImpl<value, cmplx, 48000> *>(h.impl);
            result.packets_written = dec->decode_chunk(pcm);
        }
        else
        {
            result.error = "Unsupported sample rate.";
            return result;
        }

        result.ok = true;
        return result;
    }
    catch (const std::exception &e)
    {
        result.error = e.what();
        return result;
    }
}