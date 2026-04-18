import sys
import json
import queue
import threading
from pathlib import Path

import numpy as np
import sounddevice as sd
import soundfile as sf

sys.path.append(str(Path(__file__).resolve().parent / "modem"))
import modem_tx
import modem_rx

from PySide6.QtCore import Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from core.modem_config import ModemConfig
from ui.audio_panel import AudioPanel
from ui.modem_params_panel import ModemParamsPanel
from ui.monitoring_panel import MonitoringPanel
from ui.transport_panel import TransportPanel


class TxWorker(threading.Thread):
    def __init__(
        self,
        cfg,
        input_file: str,
        output_wav: str,
        pcm_queue: queue.Queue,
        stop_event: threading.Event,
    ) -> None:
        super().__init__(daemon=True)
        self.cfg = cfg
        self.input_file = input_file
        self.output_wav = output_wav
        self.pcm_queue = pcm_queue
        self.stop_event = stop_event
        self.error: str | None = None
        self.session = None

    def run(self) -> None:
        wav = None
        try:
            payload_bytes = modem_tx.max_payload_bytes_for_config(self.cfg)
            self.session = modem_tx.create_encoder_session(self.cfg)

            wav = sf.SoundFile(
                self.output_wav,
                mode="w",
                samplerate=self.cfg.sample_rate,
                channels=self.cfg.channels,
                subtype="FLOAT",
            )

            with open(self.input_file, "rb") as f:
                while not self.stop_event.is_set():
                    chunk = f.read(payload_bytes)
                    if not chunk:
                        break

                    res = modem_tx.encode_chunk_to_pcm(self.session, list(chunk))
                    if not res.ok:
                        self.error = res.error or "encode_chunk_to_pcm failed"
                        break

                    pcm = np.asarray(res.pcm, dtype=np.float32).reshape(-1, self.cfg.channels)

                    wav.write(pcm)
                    self.pcm_queue.put(pcm)

        except Exception as e:
            self.error = str(e)
        finally:
            try:
                if self.session is not None:
                    modem_tx.destroy_encoder_session(self.session)
            except Exception:
                pass

            if wav is not None:
                wav.close()

            self.pcm_queue.put(None)


class RxWorker(threading.Thread):
    def __init__(self, cfg, output_file, pcm_queue, stop_event):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.output_file = output_file
        self.pcm_queue = pcm_queue
        self.stop_event = stop_event
        self.session = None
        self.error = None

    def run(self):
        try:
            dec_cfg = modem_rx.DecodeConfig()
            dec_cfg.sample_rate = self.cfg.sample_rate
            dec_cfg.channels = self.cfg.channels
            dec_cfg.center_freq_hz = self.cfg.center_freq_hz
            dec_cfg.output_path = self.output_file

            self.session = modem_rx.create_decoder_session(dec_cfg)

            while not self.stop_event.is_set():
                try:
                    pcm = self.pcm_queue.get(timeout=0.1)
                except queue.Empty:
                    continue

                if pcm is None:
                    break

                flat = pcm.astype(np.float32).ravel().tolist()
                res = modem_rx.decode_chunk_to_file(self.session, flat)

                if not res.ok:
                    self.error = res.error
                    break

        except Exception as e:
            self.error = str(e)
        finally:
            if self.session is not None:
                modem_rx.destroy_decoder_session(self.session)


class RealtimePlayer:
    def __init__(
        self,
        samplerate: int,
        channels: int,
        device: int,
        pcm_queue: queue.Queue,
        gain: float,
    ) -> None:
        self.samplerate = samplerate
        self.channels = channels
        self.device = device
        self.pcm_queue = pcm_queue
        self.gain = gain

        self.pending = np.empty((0, channels), dtype=np.float32)
        self.finished = False

        self.stream = sd.OutputStream(
            samplerate=samplerate,
            channels=channels,
            dtype="float32",
            device=device,
            callback=self.callback,
            blocksize=1024,
        )

    def callback(self, outdata, frames, time_info, status) -> None:
        if status:
            print("Audio status:", status)

        while len(self.pending) < frames and not self.finished:
            try:
                item = self.pcm_queue.get_nowait()
            except queue.Empty:
                break

            if item is None:
                self.finished = True
                break

            if len(self.pending) == 0:
                self.pending = item
            else:
                self.pending = np.vstack((self.pending, item))

        outdata.fill(0)

        if len(self.pending) >= frames:
            chunk = self.pending[:frames]
            self.pending = self.pending[frames:]
            outdata[:] = chunk * self.gain
        elif len(self.pending) > 0:
            outdata[:len(self.pending)] = self.pending * self.gain
            self.pending = np.empty((0, self.channels), dtype=np.float32)

    def start(self) -> None:
        self.stream.start()

    def stop(self) -> None:
        self.stream.stop()
        self.stream.close()


class RealtimeRecorder:
    def __init__(
        self,
        samplerate: int,
        channels: int,
        device: int,
        pcm_queue: queue.Queue,
    ) -> None:
        self.samplerate = samplerate
        self.channels = channels
        self.device = device
        self.pcm_queue = pcm_queue

        self.stream = sd.InputStream(
            samplerate=samplerate,
            channels=channels,
            dtype="float32",
            device=device,
            callback=self.callback,
            blocksize=1024,
        )

    def callback(self, indata, frames, time_info, status) -> None:
        if status:
            print("RX audio status:", status)

        try:
            self.pcm_queue.put_nowait(indata.copy())
        except queue.Full:
            pass

    def start(self) -> None:
        self.stream.start()

    def stop(self) -> None:
        self.stream.stop()
        self.stream.close()


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("Аудио-модем")
        self.resize(1300, 800)

        self.tx_worker: TxWorker | None = None
        self.tx_player: RealtimePlayer | None = None
        self.tx_queue: queue.Queue | None = None
        self.tx_stop_event: threading.Event | None = None
        self.rx_worker: RxWorker | None = None
        self.rx_recorder: RealtimeRecorder | None = None
        self.rx_queue: queue.Queue | None = None
        self.rx_stop_event: threading.Event | None = None
        self.rx_output_file: str | None = None

        self._build_ui()
        self._build_menu()

    def _build_ui(self) -> None:
        root = QWidget()
        root_layout = QHBoxLayout(root)

        self.params_panel = ModemParamsPanel()
        self.params_panel.setMinimumWidth(420)
        root_layout.addWidget(self.params_panel, 0)

        center = QWidget()
        center_layout = QVBoxLayout(center)

        tabs = QTabWidget()

        modem_tab = QWidget()
        modem_layout = QVBoxLayout(modem_tab)

        self.audio_panel = AudioPanel()
        modem_layout.addWidget(self.audio_panel)

        self.transport_panel = TransportPanel()
        modem_layout.addWidget(self.transport_panel, 1)

        transport_stub = QWidget()
        transport_layout = QVBoxLayout(transport_stub)
        transport_layout.addWidget(
            QLabel("Здесь можно разместить расширенные настройки proxy/TCP/UDP pipeline.")
        )
        transport_layout.addStretch(1)

        monitoring_tab = MonitoringPanel()

        tabs.addTab(modem_tab, "Modem")
        tabs.addTab(transport_stub, "Transport")
        tabs.addTab(monitoring_tab, "Monitoring")
        center_layout.addWidget(tabs, 1)

        controls = QHBoxLayout()
        self.start_tx_btn = QPushButton("START TX")
        self.start_rx_btn = QPushButton("START RX")
        self.stop_btn = QPushButton("STOP")
        self.stop_btn.setEnabled(False)

        self.start_tx_btn.setMinimumHeight(48)
        self.start_rx_btn.setMinimumHeight(48)
        self.stop_btn.setMinimumHeight(48)

        controls.addWidget(self.start_tx_btn)
        controls.addWidget(self.start_rx_btn)
        controls.addWidget(self.stop_btn)
        controls.addStretch(1)
        center_layout.addLayout(controls)

        root_layout.addWidget(center, 1)
        self.setCentralWidget(root)

        self.start_tx_btn.clicked.connect(self.on_start_tx)
        self.start_rx_btn.clicked.connect(self.on_start_rx)
        self.stop_btn.clicked.connect(self.on_stop)

    def _build_menu(self) -> None:
        file_menu = self.menuBar().addMenu("Файл")

        open_action = QAction("Открыть профиль", self)
        save_action = QAction("Сохранить профиль", self)
        exit_action = QAction("Выход", self)

        open_action.triggered.connect(self.load_profile)
        save_action.triggered.connect(self.save_profile)
        exit_action.triggered.connect(self.close)

        file_menu.addAction(open_action)
        file_menu.addAction(save_action)
        file_menu.addSeparator()
        file_menu.addAction(exit_action)

        help_menu = self.menuBar().addMenu("Справка")
        about_action = QAction("О программе", self)
        about_action.triggered.connect(self._show_about)
        help_menu.addAction(about_action)

    def _show_about(self) -> None:
        QMessageBox.about(
            self,
            "О программе",
            "Аудио-модем\nPySide6 UI + C++ core",
        )

    def save_profile(self) -> None:
        try:
            config = self.params_panel.get_config()
            filename, _ = QFileDialog.getSaveFileName(
                self, "Сохранить профиль", "", "JSON Files (*.json)"
            )
            if not filename:
                return

            Path(filename).write_text(
                json.dumps(config.to_dict(), indent=2, ensure_ascii=False),
                encoding="utf-8",
            )
            self.statusBar().showMessage("Профиль сохранён", 3000)
        except Exception as e:
            QMessageBox.critical(self, "Ошибка сохранения профиля", str(e))

    def load_profile(self) -> None:
        try:
            filename, _ = QFileDialog.getOpenFileName(
                self, "Загрузить профиль", "", "JSON Files (*.json)"
            )
            if not filename:
                return

            data = json.loads(Path(filename).read_text(encoding="utf-8"))
            config = ModemConfig.from_dict(data)
            self.params_panel.set_config(config)
            self.statusBar().showMessage("Профиль загружен", 3000)
        except Exception as e:
            QMessageBox.critical(self, "Ошибка загрузки профиля", str(e))

    def build_stream_cfg(self, ui_cfg) -> modem_tx.EncodeConfig:
        cfg = modem_tx.EncodeConfig()
        cfg.output_path = ""
        cfg.sample_rate = ui_cfg.sample_rate
        cfg.bits = ui_cfg.bits
        cfg.channels = ui_cfg.channels
        cfg.center_freq_hz = ui_cfg.carrier_freq
        cfg.callsign = "TEST"
        cfg.modulation = ui_cfg.ofdm_constellation.replace("-", "")
        cfg.code_rate = ui_cfg.ofdm_code_rate
        cfg.frame_size = ui_cfg.frame_size
        cfg.input_files = []
        return cfg

    def build_decode_cfg(self, ui_cfg) -> modem_rx.DecodeConfig:
        cfg = modem_rx.DecodeConfig()
        cfg.sample_rate = ui_cfg.sample_rate
        cfg.channels = ui_cfg.channels
        cfg.center_freq_hz = ui_cfg.carrier_freq
        cfg.output_path = ""
        return cfg

    def on_start_tx(self) -> None:
        try:
            ui_cfg = self.params_panel.get_config()

            input_file = self.transport_panel.get_input_file()
            if not input_file:
                QMessageBox.warning(self, "Ошибка", "Нет входного файла")
                return

            tx_device_id = self.audio_panel.get_selected_tx_device_id()
            if tx_device_id is None:
                QMessageBox.warning(self, "Ошибка", "Нет TX устройства")
                return

            output_file, _ = QFileDialog.getSaveFileName(self, "WAV", "out.wav")
            if not output_file:
                return

            cfg = self.build_stream_cfg(ui_cfg)

            self.tx_queue = queue.Queue(maxsize=16)
            self.tx_stop_event = threading.Event()

            self.tx_worker = TxWorker(
                cfg=cfg,
                input_file=input_file,
                output_wav=output_file,
                pcm_queue=self.tx_queue,
                stop_event=self.tx_stop_event,
            )

            self.tx_player = RealtimePlayer(
                samplerate=cfg.sample_rate,
                channels=cfg.channels,
                device=tx_device_id,
                pcm_queue=self.tx_queue,
                gain=self.audio_panel.tx_level.value() / 100.0,
            )

            self.tx_player.start()
            self.tx_worker.start()

        except Exception as e:
            QMessageBox.critical(self, "Ошибка", str(e))

    def on_start_rx(self) -> None:
        try:
            ui_cfg = self.params_panel.get_config()

            if ui_cfg.modulation_type != "OFDM":
                QMessageBox.warning(
                    self,
                    "Не поддерживается",
                    "Сейчас потоковый RX реализован только для OFDM.",
                )
                return

            rx_device_id = self.audio_panel.get_selected_rx_device_id()
            if rx_device_id is None:
                QMessageBox.warning(self, "Ошибка", "Не выбрано устройство RX.")
                return

            output_file = self.transport_panel.get_output_file()
            if not output_file:
                QMessageBox.warning(
                    self,
                    "Файл не выбран",
                    "Выберите в панели Transport назначение RX = 'Файл' и укажите путь сохранения.",
                )
                return

            cfg = self.build_decode_cfg(ui_cfg)

            self.rx_queue = queue.Queue(maxsize=32)
            self.rx_stop_event = threading.Event()
            self.rx_output_file = output_file

            self.rx_worker = RxWorker(
                cfg=cfg,
                output_file=output_file,
                pcm_queue=self.rx_queue,
                stop_event=self.rx_stop_event,
            )

            self.rx_recorder = RealtimeRecorder(
                samplerate=cfg.sample_rate,
                channels=cfg.channels,
                device=rx_device_id,
                pcm_queue=self.rx_queue,
            )

            self.rx_worker.start()
            self.rx_recorder.start()

            self.start_rx_btn.setEnabled(False)
            self.stop_btn.setEnabled(True)
            self.statusBar().showMessage("RX запущен", 3000)

        except Exception as e:
            QMessageBox.critical(self, "Ошибка запуска RX", str(e))

    def on_stop(self) -> None:
        try:
            if self.tx_stop_event is not None:
                self.tx_stop_event.set()
            if self.rx_stop_event is not None:
                self.rx_stop_event.set()

            if self.tx_worker is not None and self.tx_worker.is_alive():
                self.tx_worker.join(timeout=2.0)

            if self.rx_queue is not None:
                try:
                    self.rx_queue.put_nowait(None)
                except queue.Full:
                    pass

            if self.rx_worker is not None and self.rx_worker.is_alive():
                self.rx_worker.join(timeout=2.0)

            if self.tx_player is not None:
                self.tx_player.stop()

            if self.rx_recorder is not None:
                self.rx_recorder.stop()

            if self.tx_worker is not None and self.tx_worker.error:
                QMessageBox.critical(self, "Ошибка TX", self.tx_worker.error)

            if self.rx_worker is not None and self.rx_worker.error:
                QMessageBox.critical(self, "Ошибка RX", self.rx_worker.error)

        finally:
            self.tx_worker = None
            self.tx_player = None
            self.tx_queue = None
            self.tx_stop_event = None

            self.rx_worker = None
            self.rx_recorder = None
            self.rx_queue = None
            self.rx_stop_event = None
            self.rx_output_file = None

            self.start_tx_btn.setEnabled(True)
            self.start_rx_btn.setEnabled(True)
            self.stop_btn.setEnabled(False)
            self.statusBar().showMessage("Остановлено", 3000)


if __name__ == "__main__":
    app = QApplication([])
    window = MainWindow()
    window.show()
    app.exec()