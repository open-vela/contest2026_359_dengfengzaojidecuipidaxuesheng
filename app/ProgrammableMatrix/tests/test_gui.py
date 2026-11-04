import json
import queue
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
from client.board_layout import (DIRECT_HEADER, GPIO_LIST, RouteDesign,
                                 connector_node, connector_rows, node_label, validate_nets)
from client.jbox_gui import DeviceWorker, MatrixApp


class BoardLayoutTests(unittest.TestCase):
    def test_all_physical_pairs_share_exactly_one_node(self):
        for connector, offset in (('CN1', 0), ('CN2', 20)):
            for odd in range(1, 40, 2):
                self.assertEqual(connector_node(connector, odd), offset + (odd - 1) // 2)
                self.assertEqual(connector_node(connector, odd), connector_node(connector, odd + 1))
            rows = connector_rows(connector)
            self.assertEqual([pin for pin, node in rows[0]], list(range(39, 0, -2)))
            self.assertEqual([pin for pin, node in rows[1]], list(range(40, 0, -2)))
            self.assertEqual([node for pin, node in rows[0]], [node for pin, node in rows[1]])
        self.assertEqual(node_label(19), 'CN1 · 39/40')
        self.assertEqual(node_label(20), 'CN2 · 1/2')
        self.assertEqual(DIRECT_HEADER, ((None, 48, 47, 42, 41), (18, 21, 38, 39, 40)))

    def test_move_alias_to_another_net_and_round_trip(self):
        design = RouteDesign()
        design.toggle(connector_node('CN1', 39))
        design.active = 3
        design.toggle(connector_node('CN1', 40))
        self.assertEqual(design.nets[0], [])
        self.assertEqual(design.nets[3], [19])
        design.toggle(connector_node('CN2', 39))
        restored = RouteDesign()
        restored.import_document(json.loads(json.dumps(design.export())))
        self.assertEqual(restored.nets, design.nets)

    def test_invalid_routes_are_atomic_and_match_firmware_policy(self):
        design = RouteDesign()
        design.load([[0, 40]])
        old = [net[:] for net in design.nets]
        for node in (41, 49, 50, 51):
            with self.assertRaises(ValueError):
                design.toggle(node)
            self.assertEqual(design.nets, old)
        for document in ([[0], [0]], [[True]], [[52]], [[49, 51]], [[]] * 9):
            with self.assertRaises(ValueError):
                validate_nets(document)
        self.assertEqual(validate_nets([[0, 20, 49]])[0], [0, 20, 49])


class FakeClient:
    def __init__(self, host):
        self.host = host
        self.nets = [[] for _ in range(8)]
        self.closed = False
        self.calls = []
        self.levels = {i: 0 for i in range(9, 18)}

    def connect(self):
        self.calls.append('connect')

    def caps(self):
        return dict(nodes=52, io_gpios=list(GPIO_LIST))

    def status(self):
        self.calls.append('status')
        return dict(nets=[net[:] for net in self.nets])

    def gpio_read(self, ids):
        return {io: self.levels[io] for io in ids}

    def route_set(self, nets):
        self.calls.append('route_set')
        self.nets = [net[:] for net in nets]

    def gpio_config(self, io, mode, initial):
        self.calls.append(('gpio_config', io, mode, initial))
        self.levels[io] = initial

    def safe(self):
        self.calls.append('safe')
        self.nets = [[] for _ in range(8)]

    def close(self):
        self.closed = True


class WorkerTests(unittest.TestCase):
    def test_persistent_session_apply_readback_gpio_and_release(self):
        worker = DeviceWorker(FakeClient)
        def run(action, **args):
            worker.submit(action, **args)
            got, result, error = worker.results.get(timeout=2)
            self.assertEqual(got, action)
            self.assertIsNone(error)
            return result
        try:
            with patch('client.jbox_gui.api.discover_devices', return_value=[]):
                run('connect', host='127.0.0.1')
            original = worker.client
            nets = validate_nets([[0, 20], [19, 39]])
            self.assertEqual(run('apply', nets=nets)['status']['nets'], nets)
            self.assertEqual(run('poll')['status']['nets'], nets)
            self.assertIs(worker.client, original)
            self.assertEqual(original.calls.count('connect'), 1)
            result = run('gpio', io=17, mode=1, level=1)
            self.assertEqual(result['levels'][17], 1)
            self.assertFalse(any(run('safe')['status']['nets']))
            run('disconnect')
            self.assertTrue(original.closed)
        finally:
            worker.stop()
            worker.thread.join(2)


class PassiveWorker:
    def __init__(self):
        self.results = queue.Queue()
        self.client = None
        self.jobs = []

    def submit(self, action, **args):
        self.jobs.append((action, args))

    def stop(self):
        pass


class DesktopTests(unittest.TestCase):
    def test_acknowledgement_does_not_discard_edits_made_during_apply(self):
        worker = PassiveWorker()
        app = MatrixApp(worker=worker)
        try:
            app.connected = True
            app.design.load([[0, 20]])
            app.apply_design()
            submitted = [net[:] for net in app.design.nets]
            app.select_node(1)
            worker.results.put(('apply', {'status': {'nets': submitted}, 'levels': {}}, None))
            app._receive()
            self.assertEqual(app.applied[0], [0, 20])
            self.assertEqual(app.design.nets[0], [0, 1, 20])
            self.assertIn('待应用', app.connection.get())
        finally:
            app.close_app()

    def test_physical_holes_edit_only_until_apply_and_gpio_is_separate(self):
        worker = PassiveWorker()
        app = MatrixApp(worker=worker)
        try:
            app.update()
            self.assertEqual(len(app.pin_positions[19]), 2)
            # Click either physical hole; both render as one logical node.
            for expected in ([19], []):
                x, y = app.pin_positions[19][1]
                app.canvas.event_generate('<Button-1>', x=int(x), y=int(y))
                app.update()
                self.assertEqual(app.design.nets[0], expected)
            self.assertEqual(worker.jobs, [])
            app.select_node(0)
            app.select_node(20)
            app.select_group(1)
            app.select_node(19)
            # GPIO48 must select a GPIO control, never a matrix node.
            rect = app.canvas.bbox('gpio:48')
            app.canvas.event_generate('<Button-1>', x=(rect[0] + rect[2]) // 2,
                                      y=(rect[1] + rect[3]) // 2)
            app.update()
            self.assertEqual(app.selected_gpio, 48)
            self.assertEqual(app.design.nets[:2], [[0, 20], [19]])
            app.connected = True
            app.refresh()
            app.apply_button.invoke()
            self.assertEqual(worker.jobs[0], ('apply', {'nets': [[0, 20], [19]] + [[] for _ in range(6)]}))
            self.assertEqual(str(app.apply_button['state']), 'disabled')
        finally:
            app.close_app()


if __name__ == '__main__':
    unittest.main()
