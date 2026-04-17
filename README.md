# ofdm_modem

Simple real-time streaming OFDM modem (C++ + Python UI).

## Requirements

* Python 3
* pybind11
* numpy
* sounddevice
* soundfile
* PySide6

Установка:

```bash
pip install pybind11 numpy sounddevice soundfile PySide6
```

## Build

Собрать C++ ядро:

```bash
cd modem
make
```

## Run

Запуск приложения из корня проекта:

```bash
python main.py
```

