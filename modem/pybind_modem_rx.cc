#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "decode_engine.hh"

namespace py = pybind11;

PYBIND11_MODULE(modem_rx, m)
{
    py::class_<DecodeConfig>(m, "DecodeConfig")
        .def(py::init<>())
        .def_readwrite("sample_rate", &DecodeConfig::sample_rate)
        .def_readwrite("channels", &DecodeConfig::channels)
        .def_readwrite("center_freq_hz", &DecodeConfig::center_freq_hz)
        .def_readwrite("output_path", &DecodeConfig::output_path);

    py::class_<DecodeChunkResult>(m, "DecodeChunkResult")
        .def_readonly("ok", &DecodeChunkResult::ok)
        .def_readonly("error", &DecodeChunkResult::error)
        .def_readonly("packets_written", &DecodeChunkResult::packets_written);

    py::class_<DecoderSessionHandle>(m, "DecoderSessionHandle")
        .def_readonly("sample_rate", &DecoderSessionHandle::sample_rate);

    m.def("create_decoder_session", &create_decoder_session);
    m.def("destroy_decoder_session", &destroy_decoder_session);
    m.def("decode_chunk_to_file", &decode_chunk_to_file);
}