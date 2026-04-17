from dataclasses import dataclass, asdict
from typing import Any, Dict


@dataclass
class ModemConfig:
    modulation_type: str
    sample_rate: int
    carrier_freq: int
    bits: int
    channels: int
    tx_gain: float

    ofdm_subcarriers: int
    ofdm_cp: str
    ofdm_constellation: str
    ofdm_code_rate: int
    ofdm_fec: str
    frame_size: int

    fsk_mark: int
    fsk_space: int
    fsk_deviation: int

    psk_order: str
    psk_rolloff: float

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: Dict[str, Any]) -> "ModemConfig":
        return cls(**data)