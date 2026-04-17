from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
)
from core.modem_config import ModemConfig

class ModemParamsPanel(QGroupBox):
    def __init__(self) -> None:
        super().__init__("Параметры модуляции")
        self._build_ui()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        form = QFormLayout()
        form.setLabelAlignment(Qt.AlignLeft)
        form.setFormAlignment(Qt.AlignTop)

        self.modulation_type = QComboBox()
        self.modulation_type.addItems(["OFDM", "FSK", "PSK", "QAM"])
        self.modulation_type.currentTextChanged.connect(self._update_visibility)

        self.sample_rate = QComboBox()
        self.sample_rate.addItems(["44100", "48000"])
        self.sample_rate.setCurrentText("48000")

        self.bits = QComboBox()
        self.bits.addItem("8 bit", 8)
        self.bits.addItem("16 bit", 16)
        self.bits.addItem("32 bit", 32)
        self.bits.setCurrentIndex(0)

        self.channels = QComboBox()
        self.channels.addItem("Mono (1)", 1)
        self.channels.addItem("Stereo (2)", 2)
        self.channels.setCurrentIndex(1)

        self.carrier_freq = QSpinBox()
        self.carrier_freq.setRange(1200, 22800)
        self.carrier_freq.setSingleStep(300)
        self.carrier_freq.setValue(12000)
        self.carrier_freq.setSuffix(" Hz")


        self.tx_gain = QDoubleSpinBox()
        self.tx_gain.setRange(0.01, 5.0)
        self.tx_gain.setSingleStep(0.05)
        self.tx_gain.setValue(0.80)

        form.addRow("Частота дискретизации", self.sample_rate)
        form.addRow("Битность WAV", self.bits)
        form.addRow("Каналы", self.channels)
        form.addRow("Центральная частота", self.carrier_freq)
        form.addRow("Усиление сигнала", self.tx_gain)
        layout.addLayout(form)

        self.ofdm_group = QGroupBox("Доп. настройки OFDM")
        ofdm_form = QFormLayout(self.ofdm_group)

        self.ofdm_subcarriers = QSpinBox()
        self.ofdm_subcarriers.setRange(8, 4096)
        self.ofdm_subcarriers.setValue(64)

        self.ofdm_cp = QComboBox()
        self.ofdm_cp.addItems(["1/32", "1/16", "1/8", "1/4"])
        self.ofdm_cp.setCurrentText("1/8")

        self.ofdm_constellation = QComboBox()
        self.ofdm_constellation.addItems(["BPSK", "QPSK", "8PSK", "QAM16", "QAM64",  "QAM256",  "QAM1024",  "QAM4096"])

        self.ofdm_code_rate = QComboBox()
        self.ofdm_code_rate.addItems(["1/2", "2/3", "3/4", "5/6"])
        self.ofdm_code_rate.setCurrentText("1/2")

        self.ofdm_fec = QComboBox()
        self.ofdm_fec.addItems(["None"])

        self.frame_size = QComboBox()
        self.frame_size.addItems(["short", "normal"])
        self.frame_size.setCurrentText("short")

        ofdm_form.addRow("Модуляция поднесущих", self.ofdm_constellation)
        ofdm_form.addRow("Code Rate", self.ofdm_code_rate)
        ofdm_form.addRow("Размер кадра", self.frame_size)
        layout.addWidget(self.ofdm_group)

        self.fsk_group = QGroupBox("Доп. настройки FSK")
        fsk_form = QFormLayout(self.fsk_group)

        self.fsk_mark = QSpinBox()
        self.fsk_mark.setRange(100, 48000)
        self.fsk_mark.setValue(1800)
        self.fsk_mark.setSuffix(" Hz")

        self.fsk_space = QSpinBox()
        self.fsk_space.setRange(100, 48000)
        self.fsk_space.setValue(1200)
        self.fsk_space.setSuffix(" Hz")

        self.fsk_deviation = QSpinBox()
        self.fsk_deviation.setRange(1, 10000)
        self.fsk_deviation.setValue(300)
        self.fsk_deviation.setSuffix(" Hz")

        fsk_form.addRow("Mark", self.fsk_mark)
        fsk_form.addRow("Space", self.fsk_space)
        fsk_form.addRow("Deviation", self.fsk_deviation)
        layout.addWidget(self.fsk_group)

        self.psk_group = QGroupBox("Доп. настройки PSK")
        psk_form = QFormLayout(self.psk_group)

        self.psk_order = QComboBox()
        self.psk_order.addItems(["BPSK", "QPSK", "8-PSK"])
        self.psk_rolloff = QDoubleSpinBox()
        self.psk_rolloff.setRange(0.01, 1.0)
        self.psk_rolloff.setSingleStep(0.05)
        self.psk_rolloff.setValue(0.35)

        psk_form.addRow("Порядок", self.psk_order)
        psk_form.addRow("Rolloff", self.psk_rolloff)
        layout.addWidget(self.psk_group)

        buttons = QHBoxLayout()
        self.save_profile_btn = QPushButton("Сохранить профиль")
        self.load_profile_btn = QPushButton("Загрузить профиль")
        self.reset_profile_btn = QPushButton("Сбросить к пресету")
        buttons.addWidget(self.save_profile_btn)
        buttons.addWidget(self.load_profile_btn)
        buttons.addWidget(self.reset_profile_btn)
        layout.addLayout(buttons)
        layout.addStretch(1)

        self._update_visibility(self.modulation_type.currentText())

    def _update_visibility(self, modulation: str) -> None:
        self.ofdm_group.setVisible(modulation == "OFDM")
        self.fsk_group.setVisible(modulation == "FSK")
        self.psk_group.setVisible(modulation == "PSK")
    def get_config(self) -> ModemConfig:
        return ModemConfig(
            modulation_type=self.modulation_type.currentText(),
            sample_rate=int(self.sample_rate.currentText()),
            bits=int(self.bits.currentData()),
            channels=int(self.channels.currentData()),
            carrier_freq=self.carrier_freq.value(),
            tx_gain=self.tx_gain.value(),
            ofdm_subcarriers=self.ofdm_subcarriers.value(),
            ofdm_cp=self.ofdm_cp.currentText(),
            ofdm_constellation=self.ofdm_constellation.currentText(),
            ofdm_code_rate=self.ofdm_code_rate.currentText(),
            ofdm_fec=self.ofdm_fec.currentText(),
            frame_size=self.frame_size.currentText(),
            fsk_mark=self.fsk_mark.value(),
            fsk_space=self.fsk_space.value(),
            fsk_deviation=self.fsk_deviation.value(),
            psk_order=self.psk_order.currentText(),
            psk_rolloff=self.psk_rolloff.value(),
    )

    def set_config(self, config: ModemConfig) -> None:
        self.modulation_type.setCurrentText(config.modulation_type)
        self.sample_rate.setCurrentText(str(config.sample_rate))

        bits_index = self.bits.findData(config.bits)
        if bits_index >= 0:
            self.bits.setCurrentIndex(bits_index)

        channels_index = self.channels.findData(config.channels)
        if channels_index >= 0:
            self.channels.setCurrentIndex(channels_index)

        self.carrier_freq.setValue(config.carrier_freq)
        self.tx_gain.setValue(config.tx_gain)

        self.ofdm_subcarriers.setValue(config.ofdm_subcarriers)
        self.ofdm_cp.setCurrentText(config.ofdm_cp)
        self.ofdm_constellation.setCurrentText(config.ofdm_constellation)
        self.ofdm_code_rate.setCurrentText(config.ofdm_code_rate)
        self.ofdm_fec.setCurrentText(config.ofdm_fec)
        self.frame_size.setCurrentText(config.frame_size)

        self.fsk_mark.setValue(config.fsk_mark)
        self.fsk_space.setValue(config.fsk_space)
        self.fsk_deviation.setValue(config.fsk_deviation)

        self.psk_order.setCurrentText(config.psk_order)
        self.psk_rolloff.setValue(config.psk_rolloff)

        self._update_visibility(config.modulation_type)