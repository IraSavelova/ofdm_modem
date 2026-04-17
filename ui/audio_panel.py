from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QGridLayout,
    QGroupBox,
    QLabel,
    QSlider,
)
import sounddevice as sd


class AudioPanel(QGroupBox):
    def __init__(self) -> None:
        super().__init__("Аудио")
        self.tx_devices_map = {}
        self.rx_devices_map = {}
        self._build_ui()
        self.populate_audio_devices()
        self.update_status()

    def _build_ui(self) -> None:
        layout = QGridLayout(self)

        self.tx_device = QComboBox()
        self.rx_device = QComboBox()

        self.tx_level = QSlider(Qt.Horizontal)
        self.tx_level.setRange(0, 100)
        self.tx_level.setValue(50)

        self.rx_level = QSlider(Qt.Horizontal)
        self.rx_level.setRange(0, 100)
        self.rx_level.setValue(100)

        self.monitor_tx = QCheckBox("Мониторинг TX")
        self.monitor_tx.setChecked(True)
        self.monitor_rx = QCheckBox("Мониторинг RX")

        self.tx_ready = QLabel("● Не активно")
        self.rx_ready = QLabel("● Не активно")
        self.tx_ready.setStyleSheet("color: #6b7280; font-weight: 600;")
        self.rx_ready.setStyleSheet("color: #6b7280; font-weight: 600;")

        layout.addWidget(QLabel("Устройство передачи (TX)"), 0, 0)
        layout.addWidget(QLabel("Устройство приема (RX)"), 0, 1)

        layout.addWidget(self.tx_device, 1, 0)
        layout.addWidget(self.rx_device, 1, 1)

        layout.addWidget(self.tx_ready, 2, 0)
        layout.addWidget(self.rx_ready, 2, 1)

        layout.addWidget(QLabel("Громкость TX"), 3, 0)
        layout.addWidget(QLabel("Громкость RX"), 3, 1)

        layout.addWidget(self.tx_level, 4, 0)
        layout.addWidget(self.rx_level, 4, 1)

        layout.addWidget(self.monitor_tx, 5, 0)
        layout.addWidget(self.monitor_rx, 5, 1)

        self.tx_device.currentIndexChanged.connect(self.update_status)
        self.rx_device.currentIndexChanged.connect(self.update_status)

    def populate_audio_devices(self) -> None:
        self.tx_device.clear()
        self.rx_device.clear()

        self.tx_devices_map.clear()
        self.rx_devices_map.clear()

        try:
            devices = sd.query_devices()
        except Exception as e:
            self.tx_ready.setText("● Ошибка чтения устройств")
            self.rx_ready.setText("● Ошибка чтения устройств")
            self.tx_ready.setStyleSheet("color: #dc2626; font-weight: 600;")
            self.rx_ready.setStyleSheet("color: #dc2626; font-weight: 600;")
            print(f"Ошибка sounddevice: {e}")
            return

        for i, dev in enumerate(devices):
            name = dev["name"]

            # TX = воспроизведение = output
            if dev["max_output_channels"] > 0:
                label = name
                self.tx_device.addItem(label)
                self.tx_devices_map[label] = i

            # RX = захват = input
            if dev["max_input_channels"] > 0:
                label = name
                self.rx_device.addItem(label)
                self.rx_devices_map[label] = i

        self.select_virtual_devices()

    def select_virtual_devices(self) -> None:
        for label in self.tx_devices_map:
            low = label.lower()
            if "vb" in low or "cable" in low or "blackhole" in low:
                self.tx_device.setCurrentText(label)
                break

        for label in self.rx_devices_map:
            low = label.lower()
            if "vb" in low or "cable" in low or "blackhole" in low:
                self.rx_device.setCurrentText(label)
                break

    def get_selected_tx_device_id(self):
        label = self.tx_device.currentText()
        return self.tx_devices_map.get(label)

    def get_selected_rx_device_id(self):
        label = self.rx_device.currentText()
        return self.rx_devices_map.get(label)
    
    def check_device(self, device_id, is_input):
        try:
            if is_input:
                with sd.InputStream(device=device_id):
                    pass
            else:
                with sd.OutputStream(device=device_id):
                    pass
            return True
        except Exception as e:
            print(f"Ошибка проверки устройства {device_id}: {e}")
            return False
    def update_status(self):
        tx_id = self.get_selected_tx_device_id()
        rx_id = self.get_selected_rx_device_id()

        if tx_id is not None and self.check_device(tx_id, is_input=False):
            self.tx_ready.setText("● Подключено")
            self.tx_ready.setStyleSheet("color: #2e9d46; font-weight: 600;")
        else:
            self.tx_ready.setText("● Ошибка")
            self.tx_ready.setStyleSheet("color: #dc2626; font-weight: 600;")

        if rx_id is not None and self.check_device(rx_id, is_input=True):
            self.rx_ready.setText("● Подключено")
            self.rx_ready.setStyleSheet("color: #2e9d46; font-weight: 600;")
        else:
            self.rx_ready.setText("● Ошибка")
            self.rx_ready.setStyleSheet("color: #dc2626; font-weight: 600;")