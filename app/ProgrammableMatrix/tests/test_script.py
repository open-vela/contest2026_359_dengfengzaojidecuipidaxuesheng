import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch, call
from types import SimpleNamespace
import queue

sys.path.insert(0, str(Path(__file__).parents[1]))
from client.jbox_lang import compile_function, OPS
from client.jbox_api import MatrixClient, APIError, CMD_FUNCTION_LOAD, CMD_FUNCTION_CALL
from client.dht11_viewer import configure_sensor, read_values, DHT_ROUTES, PROGRAM, DHTWindow


class LanguageTests(unittest.TestCase):
    def test_uploaded_program_contains_full_dht_decode(self):
        function = compile_function(PROGRAM.read_text())
        instructions = list(struct.iter_unpack('<BBH', function.code))
        self.assertEqual(function.parameters, ('data',))
        self.assertEqual(sum(op == OPS['measure_low'] for op, _, _ in instructions), 40)
        self.assertEqual(sum(op == OPS['assert'] for op, _, _ in instructions), 5)
        self.assertEqual(sum(op == OPS['emit_value'] for op, _, _ in instructions), 2)

    def test_compiler_rejects_unbounded_or_invalid_code(self):
        sources = [
            'fn x(p){ while(1) { high(p); } }',
            'fn x(p){ repeat(0){high(p);} }',
            'fn x(p){ timed {sleep_ms(20);} }',
            'fn x(p){ timed {output(p);} }',
            'fn x(p){ timed { timed { high(p); } } }',
            'fn x(p){ timed {delay_us(10000);} }',
            'fn x(p){ repeat(128){repeat(128){high(p);}} }',
            'fn x(p){emit(undefined);}',
            'fn x(p){let a=a+1;}',
            'fn x(p){repeat(128){emit(1);emit(2);}}',
            'fn x(p){low(other);}',
        ]
        for source in sources:
            with self.subTest(source=source), self.assertRaises(ValueError):
                compile_function(source)

    def test_remote_function_upload_call_and_arguments(self):
        client = object.__new__(MatrixClient)
        client._rpc = Mock(return_value=b'')
        function = client.upload_function('fn probe(pin){emit(sample(pin));}', slot=2)
        cmd, payload = client._rpc.call_args.args
        self.assertEqual(cmd, CMD_FUNCTION_LOAD)
        self.assertEqual(struct.unpack_from('<BBH', payload), (2, 1, 2))
        client._rpc.return_value = struct.pack('<HI', 1, 1)
        self.assertEqual(function(pin=9), [1])
        self.assertEqual(client._rpc.call_args.args, (CMD_FUNCTION_CALL, bytes((2, 1, 9))))
        for args, kwargs in [((9,), {'pin': 9}), ((), {}), ((), {'other': 9})]:
            with self.assertRaises(TypeError):
                function(*args, **kwargs)
        with self.assertRaises(ValueError):
            function(pin=18)
        client._rpc.return_value = b'\x01\x00'
        with self.assertRaises(IOError):
            function(pin=9)

    def test_blank_host_selects_the_only_discovered_device(self):
        board = Mock()
        stop = Mock()
        stop.wait.return_value = True
        window = SimpleNamespace(events=queue.Queue())
        device = {'host': '192.0.2.1', 'pairing_open': False}
        with patch('client.dht11_viewer.discover_devices', return_value=[device]), \
             patch('client.dht11_viewer.MatrixClient', return_value=board) as create:
            DHTWindow.run_sensor(window, '', stop)
        create.assert_called_once_with('192.0.2.1', timeout=10)
        self.assertIn(('host', '192.0.2.1'), list(window.events.queue))
        board.close.assert_called_once()

    def test_blank_host_does_not_guess_when_discovery_is_ambiguous(self):
        for devices in ([], [{'host': '192.0.2.1'}, {'host': '192.0.2.2'}]):
            window = SimpleNamespace(events=queue.Queue())
            with patch('client.dht11_viewer.discover_devices', return_value=devices), \
                 patch('client.dht11_viewer.MatrixClient') as create:
                DHTWindow.run_sensor(window, '', Mock())
            create.assert_not_called()
            self.assertIn('sensor_error', [item[0] for item in list(window.events.queue)])

    def test_timeout_restores_idle_pullup_before_retry_wait(self):
        board = Mock()
        board.upload_function.return_value.side_effect = APIError(-6, CMD_FUNCTION_CALL)
        stop = Mock()
        stop.is_set.return_value = False
        stop.wait.side_effect = [False, True]
        window = SimpleNamespace(events=queue.Queue())
        with patch('client.dht11_viewer.discover_devices', return_value=[]), \
             patch('client.dht11_viewer.MatrixClient', return_value=board):
            DHTWindow.run_sensor(window, '127.0.0.1', stop)
        self.assertEqual(board.gpio_config.call_count, 2)
        self.assertEqual(board.gpio_config.call_args, call(0, mode=2, pull=1, initial=1))
        stop.wait.assert_called()
        board.close.assert_called_once()
        self.assertIn('sensor_error', [event[0] for event in list(window.events.queue)])

    def test_viewer_routes_and_returned_values(self):
        board = Mock()
        board.upload_function.return_value.return_value = [245, 603]
        function = configure_sensor(board)
        self.assertEqual(board.route_set.call_args.args[0], [[19, 49], [18, 40], [16, 51]])
        self.assertNotIn(17, sum(DHT_ROUTES, []))
        self.assertEqual(read_values(function), (24.5, 60.3))
        function.assert_called_once_with(data=0)
        function.return_value = [500, 1001]
        with self.assertRaises(ValueError):
            read_values(function)


if __name__ == '__main__':
    unittest.main()
