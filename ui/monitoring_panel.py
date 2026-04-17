from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QWidget,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QVBoxLayout,
)

class MonitoringPanel(QWidget):
    def __init__(self) -> None:
        super().__init__()
        self._build_ui()

    def _metric_card(self, title: str, value: str) -> QGroupBox:
        card = QGroupBox(title)
        layout = QVBoxLayout(card)
        number = QLabel(value)
        number.setStyleSheet("font-size: 22px; font-weight: 700;")
        layout.addWidget(number)
        return card

    def _placeholder_plot(self, title: str, subtitle: str) -> QGroupBox:
        box = QGroupBox(title)
        layout = QVBoxLayout(box)
        label = QLabel(subtitle)
        label.setAlignment(Qt.AlignCenter)
        label.setStyleSheet(
            "background: #0f172a; color: #e5e7eb; border: 1px solid #334155; padding: 60px;"
        )
        layout.addWidget(label)
        return box

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)

        metrics = QHBoxLayout()
        metrics.addWidget(self._metric_card("Tx bitrate", "0 bit/s"))
        metrics.addWidget(self._metric_card("Rx bitrate", "0 bit/s"))
        metrics.addWidget(self._metric_card("BER", "-"))
        metrics.addWidget(self._metric_card("Кадры", "0"))
        metrics.addWidget(self._metric_card("Ошибки", "0"))
        layout.addLayout(metrics)

        plots = QHBoxLayout()
        plots.addWidget(self._placeholder_plot("Спектр", "Здесь будет график спектра"), 1)
        plots.addWidget(self._placeholder_plot("Созвездие", "Здесь будет график созвездия"), 1)
        layout.addLayout(plots)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.appendPlainText("[12:09:42] Приложение запущено")
        self.log.appendPlainText("[12:09:42] Найдено аудиоустройств — вход: 5, выход: 6")
        self.log.appendPlainText("[12:09:42] Выбран профиль: OFDM, Fs=48000 Hz, fc=12000 Hz")
        layout.addWidget(QGroupBox(""))
        log_box = QGroupBox("Лог")
        log_layout = QVBoxLayout(log_box)
        log_layout.addWidget(self.log)
        layout.addWidget(log_box)