#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "encode_engine.hh"

namespace py = pybind11;

PYBIND11_MODULE(modem_tx, m)
{
    py::class_<EncodeConfig>(m, "EncodeConfig")
        .def(py::init<>())
        .def_readwrite("output_path", &EncodeConfig::output_path)
        .def_readwrite("sample_rate", &EncodeConfig::sample_rate)
        .def_readwrite("bits", &EncodeConfig::bits)
        .def_readwrite("channels", &EncodeConfig::channels)
        .def_readwrite("center_freq_hz", &EncodeConfig::center_freq_hz)
        .def_readwrite("callsign", &EncodeConfig::callsign)
        .def_readwrite("modulation", &EncodeConfig::modulation)
        .def_readwrite("code_rate", &EncodeConfig::code_rate)
        .def_readwrite("frame_size", &EncodeConfig::frame_size)
        .def_readwrite("input_files", &EncodeConfig::input_files);

    py::class_<EncodeResult>(m, "EncodeResult")
        .def_readonly("ok", &EncodeResult::ok)
        .def_readonly("error", &EncodeResult::error);

    py::class_<EncodeChunkResult>(m, "EncodeChunkResult")
        .def_readonly("ok", &EncodeChunkResult::ok)
        .def_readonly("error", &EncodeChunkResult::error)
        .def_readonly("pcm", &EncodeChunkResult::pcm);

    py::class_<EncoderSessionHandle>(m, "EncoderSessionHandle")
        .def(py::init<>())
        .def_readonly("sample_rate", &EncoderSessionHandle::sample_rate);

    m.def("encode_to_wav", &encode_to_wav);
    m.def("max_payload_bytes_for_config", &max_payload_bytes_for_config);
    m.def("create_encoder_session", &create_encoder_session);
    m.def("destroy_encoder_session", &destroy_encoder_session);
    m.def("encode_chunk_to_pcm", &encode_chunk_to_pcm);
}