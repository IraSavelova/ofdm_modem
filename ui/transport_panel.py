from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QWidget,
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QPlainTextEdit,
    QGridLayout,
    QLineEdit,
    QSpinBox,
    QFileDialog,
    QVBoxLayout,
)


class TransportPanel(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.selected_file_path: str | None = None
        self.selected_save_path: str | None = None
        self._build_ui()
        self._setup_connections()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        source_group = QGroupBox("Источник данных (TX)")
        source_layout = QVBoxLayout(source_group)

        top_row = QHBoxLayout()
        self.source_type = QComboBox()
        self.source_type.addItems(["Ввод", "Файл", "TCP", "UDP", "Raw Hex"])

        self.open_file_btn = QPushButton("Открыть файл")
        self.open_file_btn.setEnabled(False)

        top_row.addWidget(QLabel("Источник"))
        top_row.addWidget(self.source_type, 1)
        top_row.addWidget(self.open_file_btn)

        self.tx_payload = QPlainTextEdit()
        self.tx_payload.setPlaceholderText("Введите данные для передачи...")
        self.tx_payload.setPlainText("Hello from OFDM modem!\nThis is a test message.")

        source_layout.addLayout(top_row)
        source_layout.addWidget(self.tx_payload)
        layout.addWidget(source_group)

        self.tcp_group = QGroupBox("TCP/UDP источник")
        tcp_layout = QGridLayout(self.tcp_group)

        self.tx_mode = QComboBox()
        self.tx_mode.addItems(["TCP Server (слушать)", "TCP Client (подключиться)", "UDP"])

        self.tx_host = QLineEdit("127.0.0.1")

        self.tx_port = QSpinBox()
        self.tx_port.setRange(1, 65535)
        self.tx_port.setValue(9000)

        self.tx_start_btn = QPushButton("Старт")
        self.tx_stop_btn = QPushButton("Стоп")
        self.tx_stop_btn.setEnabled(False)

        self.tx_status = QLabel("● Ожидание подключения...")
        self.tx_status.setStyleSheet("color: #2e9d46;")

        tcp_layout.addWidget(QLabel("Режим"), 0, 0)
        tcp_layout.addWidget(self.tx_mode, 0, 1)
        tcp_layout.addWidget(QLabel("Адрес"), 0, 2)
        tcp_layout.addWidget(self.tx_host, 0, 3)
        tcp_layout.addWidget(QLabel("Порт"), 0, 4)
        tcp_layout.addWidget(self.tx_port, 0, 5)
        tcp_layout.addWidget(self.tx_start_btn, 1, 4)
        tcp_layout.addWidget(self.tx_stop_btn, 1, 5)
        tcp_layout.addWidget(self.tx_status, 1, 0, 1, 4)

        layout.addWidget(self.tcp_group)
        self.tcp_group.setVisible(False)

        rx_group = QGroupBox("Приём данных (RX)")
        rx_layout = QVBoxLayout(rx_group)

        rx_top = QHBoxLayout()
        self.rx_output_type = QComboBox()
        self.rx_output_type.addItems(["Текст", "Файл", "TCP", "UDP", "Raw Hex"])

        self.rx_save_btn = QPushButton("Сохранить в файл")
        self.rx_save_btn.setEnabled(False)

        self.rx_clear_btn = QPushButton("Очистить")

        rx_top.addWidget(QLabel("Назначение"))
        rx_top.addWidget(self.rx_output_type, 1)
        rx_top.addWidget(self.rx_save_btn)
        rx_top.addWidget(self.rx_clear_btn)

        self.rx_text = QPlainTextEdit()
        self.rx_text.setReadOnly(True)
        self.rx_text.setPlaceholderText("Ожидание данных...")

        self.rx_stats = QLabel("Принято: 0 байт • Ошибки: 0 • Кадры: 0")

        rx_layout.addLayout(rx_top)
        rx_layout.addWidget(self.rx_text)
        rx_layout.addWidget(self.rx_stats)
        layout.addWidget(rx_group)

        layout.addStretch(1)

        self._update_file_button_text()

    def _setup_connections(self) -> None:
        self.source_type.currentTextChanged.connect(self._on_source_changed)
        self.open_file_btn.clicked.connect(self._choose_file)
        self.rx_output_type.currentTextChanged.connect(self._on_output_changed)
        self.rx_save_btn.clicked.connect(self._save_file)
        self.rx_clear_btn.clicked.connect(self._clear_rx)

    def _on_source_changed(self, source: str) -> None:
        self.open_file_btn.setEnabled(source == "Файл")
        self.tx_payload.setVisible(source == "Ввод")

        if source in ["TCP", "UDP"]:
            self.tcp_group.setVisible(True)
            if source == "UDP":
                self.tx_mode.setCurrentText("UDP")
                self.tx_mode.setEnabled(False)
                self.tx_host.setEnabled(True)
                self.tx_port.setEnabled(True)
            else:
                self.tx_mode.setEnabled(True)
                self.tx_host.setEnabled(True)
                self.tx_port.setEnabled(True)
        else:
            self.tcp_group.setVisible(False)

    def _on_output_changed(self, output: str) -> None:
        self.rx_save_btn.setEnabled(output == "Файл")
        self.rx_text.setVisible(output == "Текст")

    def _choose_file(self) -> None:
        filename, _ = QFileDialog.getOpenFileName(
            self,
            "Выбор файла",
            "",
            "All Files (*.*)"
        )
        if not filename:
            return

        self.selected_file_path = filename
        self._update_file_button_text()

    def _update_file_button_text(self) -> None:
        if self.selected_file_path:
            self.open_file_btn.setText(Path(self.selected_file_path).name)
        else:
            self.open_file_btn.setText("Открыть файл")

    def _save_file(self) -> None:
        filename, _ = QFileDialog.getSaveFileName(
            self,
            "Сохранить файл",
            "",
            "All Files (*.*)"
        )
        if not filename:
            return

        self.selected_save_path = filename

    def _clear_rx(self) -> None:
        self.rx_text.clear()

    def get_input_file(self) -> str | None:
        if self.source_type.currentText() != "Файл":
            return None
        return self.selected_file_path

    def get_source_type(self) -> str:
        return self.source_type.currentText()