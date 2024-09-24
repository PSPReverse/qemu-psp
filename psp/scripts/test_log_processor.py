import unittest
from io import StringIO
from unittest.mock import patch

from log_processor import LogProcessor


class TestProcessInputs(unittest.TestCase):

    @patch("sys.stdout", new_callable=StringIO)
    def test_grouping_adjacent_reads(self, mock_stdout):
        # Simulated input
        input_data = iter(
            [
                "psp_smn_misc_read_unimplemented offset=0x5d1d8 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1dc value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1e0 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1e4 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1e8 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1ec value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1f0 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1f4 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1f8 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d1fc value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d200 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d204 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d208 value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d20c value=0x0 (default)",
                "psp_smn_misc_read_unimplemented offset=0x5d210 value=0x0 (default)",
            ]
        )
        lp = LogProcessor()
        # Call the function with the simulated input
        lp.process_log(input_data)

        # Check the output
        expected_output = (
            "psp_smn_misc_read_unimplemented offset=0x5d1d8-0x5d210 value=[0x0*15]\n"
        )
        self.assertEqual(mock_stdout.getvalue(), expected_output)

    @patch("sys.stdout", new_callable=StringIO)
    def test_remove_region_when_unknown(self, mock_stdout):
        # Simulated input
        input_data = iter(
            [
                "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4 region=unknown offset=0xfffdfc000080\n",
            ]
        )

        lp = LogProcessor()
        # Call the function with the simulated input
        lp.process_log(input_data)

        # Check the output
        expected_output = (
            "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4\n"
        )
        self.assertEqual(mock_stdout.getvalue(), expected_output)

    @patch("sys.stdout", new_callable=StringIO)
    def test_default(self, mock_stdout):
        # Simulated input
        input_data = iter(
            [
                "psp_x86_misc_read_unimplemented addr=0xfec20040 value=0x0 (default) region=ESPI offset=0x40",
                "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4 region=unknown offset=0xfffdfc000080\n",
            ]
        )

        lp = LogProcessor()
        # Call the function with the simulated input
        lp.process_log(input_data)

        # Check the output
        expected_output = (
            "psp_x86_misc_read_unimplemented region=ESPI addr=0xfec20040 offset=0x40 value=0x0\n"
            "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4\n"
        )
        self.assertEqual(mock_stdout.getvalue(), expected_output)

    @patch("sys.stdout", new_callable=StringIO)
    def test_pci_enhancer(self, mock_stdout):
        # Simulated input
        input_data = iter(
            [
                "psp_x86_config_space_read_unimplemented bus=0x0 device=0x14 function=0x3 offset=0x48 value=0x0",
                "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4 region=unknown offset=0xfffdfc000080\n",
            ]
        )

        lp = LogProcessor()
        # Call the function with the simulated input
        lp.process_log(input_data)

        # Check the output
        expected_output = (
            "psp_x86_config_space_read_unimplemented bus=0x0 device=0x14 function=0x3 name=FCH::ITF::LPC::IO_MEM_PORT_DECODE_ENABLE offset=0x48 value=0x0\n"
            "psp_x86_misc_write_unimplemented addr=0xfffdfc000080 value=0xee0000b4\n"
        )
        self.assertEqual(mock_stdout.getvalue(), expected_output)


if __name__ == "__main__":
    unittest.main()
