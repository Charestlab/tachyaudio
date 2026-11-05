"""ALSA control enumeration without opening PCM devices or requiring Linux."""

import unittest
from unittest.mock import Mock, patch

from tachyaudio import _alsa_devices as devices


def output(**changes):
    fields = dict(
        device_id="hw:CARD=PCH,DEV=0",
        short_id="hw:0,0",
        kind="jacks",
        card_name="Onboard",
        name="Analog",
        screen_connected=None,
        in_use_by=None,
    )
    return devices.AlsaOutput(**{**fields, **changes})


class AlsaDeviceTests(unittest.TestCase):
    def test_output_labels_status_and_table_keep_selection_information(self):
        free = output()
        busy = output(in_use_by="player (pid 1)")
        disconnected = output(screen_connected=False, kind="screen")
        self.assertEqual(free.label, "Analog (Onboard)")
        self.assertEqual(output(kind="USB").label, "Onboard")
        self.assertEqual(output(name="Onboard").label, "Onboard")
        self.assertEqual(free.status, "free")
        self.assertIn("player", busy.status)
        self.assertEqual(disconnected.status, "no screen connected")
        self.assertIn(free.device_id, str(free))
        with patch.object(devices, "list_alsa_outputs", return_value=(busy, disconnected, free)):
            table = devices.describe_alsa_outputs()
        self.assertIn('device_id="hw:CARD=PCH,DEV=0"', table)
        self.assertIn("must be released first", table)
        with patch.object(devices, "list_alsa_outputs", return_value=()):
            self.assertIn("No ALSA", devices.describe_alsa_outputs())
        with patch.object(devices, "list_alsa_outputs", return_value=(disconnected,)):
            self.assertIn(disconnected.short_id, devices.describe_alsa_outputs())

    def test_device_kinds(self):
        for driver, name, screen, expected in (
            ("HDA", "Analog", None, "jacks"),
            ("USB-Audio", "Headset", None, "USB"),
            ("HDA", "SPDIF", None, "digital"),
            ("HDA", "IEC958", None, "digital"),
            ("HDA", "Digital", None, "digital"),
            ("HDA", "HDMI 0", None, "screen"),
            ("HDA", "DP 0", False, "screen"),
            ("Other", "Output", None, "other"),
        ):
            with self.subTest(name=name):
                self.assertEqual(devices._kind(driver, name, screen), expected)

    def test_holder_handles_free_missing_unknown_and_named_processes(self):
        for text, expected in (("closed\n", None), ("state: RUNNING\n", "another process")):
            with patch.object(devices.Path, "read_text", return_value=text):
                self.assertEqual(devices._holder(0, 0), expected)
        with patch.object(devices.Path, "read_text", side_effect=OSError):
            self.assertIsNone(devices._holder(0, 0))
        for command, expected in (("player\n", "player (pid 42)"), (OSError(), "process (pid 42)")):
            with patch.object(devices.Path, "read_text", side_effect=["owner_pid: 42\n", command]):
                self.assertEqual(devices._holder(0, 0), expected)

    def test_jack_connection_is_tristate(self):
        alsa = Mock()
        for result, boolean, expected in ((-1, 0, None), (0, 0, False), (0, 1, True)):
            alsa.snd_ctl_elem_read.return_value = result
            alsa.snd_ctl_elem_value_get_boolean.return_value = boolean
            self.assertIs(devices._screen_connected(alsa, "control", "value", 3), expected)
        alsa.snd_ctl_elem_value_set_name.assert_called_with("value", b"HDMI/DP,pcm=3 Jack")

    def test_missing_library_returns_empty_enumeration(self):
        with patch.object(devices.ctypes, "CDLL", side_effect=OSError):
            self.assertEqual(devices.list_alsa_outputs(), ())

    def test_control_enumeration_closes_allocations_on_success_and_failure(self):
        for failure in (None, "open", "info", "pcm"):
            with self.subTest(failure=failure):
                alsa = Mock()

                def next_card(pointer):
                    pointer._obj.value = 0 if pointer._obj.value == -1 else -1
                    return 0

                def next_device(control, pointer):
                    pointer._obj.value = 0 if pointer._obj.value == -1 else -1
                    return 0

                alsa.snd_card_next.side_effect = next_card
                alsa.snd_ctl_pcm_next_device.side_effect = next_device
                alsa.snd_ctl_open.return_value = -1 if failure == "open" else 0
                alsa.snd_ctl_card_info.return_value = -1 if failure == "info" else 0
                alsa.snd_ctl_pcm_info.return_value = -1 if failure == "pcm" else 0
                alsa.snd_ctl_card_info_get_id.return_value = b"PCH"
                alsa.snd_ctl_card_info_get_name.return_value = b"Onboard"
                alsa.snd_ctl_card_info_get_driver.return_value = b"HDA"
                alsa.snd_pcm_info_get_name.return_value = b"Analog"
                with (
                    patch.object(devices.ctypes, "CDLL", return_value=alsa),
                    patch.object(devices, "_screen_connected", return_value=None),
                    patch.object(devices, "_holder", return_value=None),
                ):
                    found = devices.list_alsa_outputs()
                self.assertEqual(len(found), 0 if failure else 1)
                if failure != "open":
                    alsa.snd_ctl_close.assert_called_once()
                    alsa.snd_ctl_card_info_free.assert_called_once()
                    alsa.snd_pcm_info_free.assert_called_once()
                    alsa.snd_ctl_elem_value_free.assert_called_once()
                if not failure:
                    self.assertEqual(found[0], output())
