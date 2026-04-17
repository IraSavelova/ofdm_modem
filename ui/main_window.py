import json
from pathlib import Path
from PySide6.QtWidgets import QFileDialog, QMessageBox

def save_profile(self) -> None:
    config = self.params_panel.get_config()
    filename, _ = QFileDialog.getSaveFileName(
        self,
        "Сохранить профиль",
        "",
        "JSON Files (*.json)"
    )
    if not filename:
        return

    Path(filename).write_text(
        json.dumps(config.to_dict(), indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

def load_profile(self) -> None:
    filename, _ = QFileDialog.getOpenFileName(
        self,
        "Загрузить профиль",
        "",
        "JSON Files (*.json)"
    )
    if not filename:
        return

    data = json.loads(Path(filename).read_text(encoding="utf-8"))
    config = ModemConfig.from_dict(data)
    self.params_panel.set_config(config)