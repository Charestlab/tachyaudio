"""Enumerate ALSA playback hardware for Linux scheduled output.

Reads card/PCM information through libasound's control interface (the same
data as ``aplay -l``) without opening any PCM, so it is safe while streams run.
"""

from __future__ import annotations

import ctypes
from dataclasses import dataclass
from pathlib import Path

_ALSA_PROC = Path("/proc/asound")


@dataclass(frozen=True, slots=True)
class AlsaOutput:
    """One ALSA playback device usable as ``ScheduledOutputStream(device_id=...)``."""

    device_id: str
    """Stable id, ``hw:CARD=<card id>,DEV=<n>``; survives USB re-plugging."""
    short_id: str
    """``hw:<card>,<device>``; card numbers can change between boots."""
    kind: str
    """``screen``, ``jacks`` (headphone/speaker sockets), ``digital``, ``USB`` or ``other``."""
    card_name: str
    name: str
    screen_connected: bool | None
    """HDMI/DisplayPort only: whether a screen is plugged in; None otherwise."""
    in_use_by: str | None
    """Name and pid of the process holding the device, or None if free."""

    @property
    def status(self) -> str:
        if self.screen_connected is False:
            return "no screen connected"
        return f"in use by {self.in_use_by}" if self.in_use_by else "free"

    @property
    def label(self) -> str:
        if self.kind == "USB" or self.name == self.card_name:
            return self.card_name
        return f"{self.name} ({self.card_name})"

    def __str__(self) -> str:
        return (f"{self.device_id:<26}{self.short_id:<8}{self.kind:<9}"
                f"{self.label:<40}{self.status}")


def list_alsa_outputs() -> tuple[AlsaOutput, ...]:
    """Return the ALSA playback devices of this computer (empty if unavailable)."""
    try:
        alsa = ctypes.CDLL("libasound.so.2")
    except OSError:
        return ()
    for name in ("snd_ctl_card_info_get_id", "snd_ctl_card_info_get_name",
                 "snd_ctl_card_info_get_driver", "snd_pcm_info_get_name"):
        getattr(alsa, name).restype = ctypes.c_char_p
    devices = []
    card = ctypes.c_int(-1)
    while alsa.snd_card_next(ctypes.byref(card)) == 0 and card.value >= 0:
        control = ctypes.c_void_p()
        if alsa.snd_ctl_open(ctypes.byref(control), f"hw:{card.value}".encode(), 0) < 0:
            continue
        card_info, pcm_info, value = ctypes.c_void_p(), ctypes.c_void_p(), ctypes.c_void_p()
        alsa.snd_ctl_card_info_malloc(ctypes.byref(card_info))
        alsa.snd_pcm_info_malloc(ctypes.byref(pcm_info))
        alsa.snd_ctl_elem_value_malloc(ctypes.byref(value))
        try:
            if alsa.snd_ctl_card_info(control, card_info) < 0:
                continue
            card_id = alsa.snd_ctl_card_info_get_id(card_info).decode(errors="replace")
            card_name = alsa.snd_ctl_card_info_get_name(card_info).decode(errors="replace")
            driver = alsa.snd_ctl_card_info_get_driver(card_info).decode(errors="replace")
            device = ctypes.c_int(-1)
            while alsa.snd_ctl_pcm_next_device(control, ctypes.byref(device)) == 0 \
                    and device.value >= 0:
                alsa.snd_pcm_info_set_device(pcm_info, device.value)
                alsa.snd_pcm_info_set_subdevice(pcm_info, 0)
                alsa.snd_pcm_info_set_stream(pcm_info, 0)  # playback
                if alsa.snd_ctl_pcm_info(control, pcm_info) < 0:
                    continue
                name = alsa.snd_pcm_info_get_name(pcm_info).decode(errors="replace")
                screen = _screen_connected(alsa, control, value, device.value)
                devices.append(AlsaOutput(
                    device_id=f"hw:CARD={card_id},DEV={device.value}",
                    short_id=f"hw:{card.value},{device.value}",
                    kind=_kind(driver, name, screen),
                    card_name=card_name,
                    name=name,
                    screen_connected=screen,
                    in_use_by=_holder(card.value, device.value),
                ))
        finally:
            alsa.snd_ctl_elem_value_free(value)
            alsa.snd_pcm_info_free(pcm_info)
            alsa.snd_ctl_card_info_free(card_info)
            alsa.snd_ctl_close(control)
    return tuple(devices)


def _kind(driver: str, name: str, screen: bool | None) -> str:
    if screen is not None or name.upper().startswith(("HDMI", "DP")):
        return "screen"
    if "USB" in driver.upper():
        return "USB"
    if "ANALOG" in name.upper():
        return "jacks"
    if "DIGITAL" in name.upper() or "SPDIF" in name.upper() or "IEC958" in name.upper():
        return "digital"
    return "other"


def _screen_connected(alsa: ctypes.CDLL, control: ctypes.c_void_p, value: ctypes.c_void_p,
                      device: int) -> bool | None:
    """Read the HDA HDMI/DisplayPort jack control for this PCM, if it exists."""
    alsa.snd_ctl_elem_value_clear(value)
    alsa.snd_ctl_elem_value_set_interface(value, 0)  # SND_CTL_ELEM_IFACE_CARD
    alsa.snd_ctl_elem_value_set_name(value, f"HDMI/DP,pcm={device} Jack".encode())
    if alsa.snd_ctl_elem_read(control, value) < 0:
        return None
    return bool(alsa.snd_ctl_elem_value_get_boolean(value, 0))


def _holder(card: int, device: int) -> str | None:
    status = _ALSA_PROC / f"card{card}" / f"pcm{device}p" / "sub0" / "status"
    try:
        text = status.read_text()
    except OSError:
        return None
    if text.strip() == "closed":
        return None
    for line in text.splitlines():
        key, _, pid = line.partition(":")
        if key.strip() == "owner_pid" and pid.strip().isdigit():
            try:
                command = Path(f"/proc/{pid.strip()}/comm").read_text().strip()
            except OSError:
                command = "process"
            return f"{command} (pid {pid.strip()})"
    return "another process"


def describe_alsa_outputs() -> str:
    """Human-readable table of playback devices, with how to choose one."""
    devices = list_alsa_outputs()
    if not devices:
        return "No ALSA playback device was found (is libasound.so.2 installed?)."
    lines = ["ALSA playback devices on this computer:", "",
             f"  {'device_id':<26}{'short':<8}{'type':<9}{'name':<40}status"]
    lines += [f"  {device}" for device in devices]
    example = next((d for d in devices if not d.in_use_by and d.screen_connected is not False),
                   devices[0])
    lines += [
        "",
        f'Pass one as device_id, e.g. device_id="{example.device_id}" '
        f'(or the shorter "{example.short_id}").',
        "Types: screen = HDMI/DisplayPort speakers of a monitor or TV (only while one is",
        "connected); jacks = the computer's headphone/speaker sockets; digital = S/PDIF;",
        "USB = USB headset or audio interface.",
    ]
    if any(d.in_use_by for d in devices):
        lines.append("A device in use must be released first, e.g. by closing the application "
                     "that plays through it (PipeWire holds a device while any app uses it).")
    return "\n".join(lines)
