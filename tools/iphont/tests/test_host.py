import contextlib
import io
import json
import os
import struct
import tempfile
import unittest
import zlib
from unittest.mock import patch

from iphont import agent, cli, device, diagnostics, link, logs, stream, symbols, workflow
from iphont.errors import Fail


class HostTests(unittest.TestCase):
    def test_cli_errors_are_json(self):
        for args in ([], ['--unknown'], ['tap', 'not-int', '2'], ['sysctl']):
            with self.subTest(args=args), contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(cli.main(args), 2)
                self.assertEqual(json.loads(out.getvalue())['error']['code'], 'usage')

    def test_command_catalog(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(cli.main(['describe']), 0)
        names = {c['name'] for c in json.loads(out.getvalue())['result']['commands']}
        self.assertTrue({'processes', 'screen', 'events', 'sysctl', 'test'} <= names)

    def test_nested_transaction_and_contention(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(link, 'LOCK', os.path.join(tmp, 'lock')):
            with link.transaction(), link.transaction():
                self.assertEqual(link._local.depth, 2)
                with self.assertRaises(Fail):
                    with link._Flock(link.LOCK, 0): pass
            with link._Flock(link.LOCK, 0): pass

    def test_payload_matches_format(self):
        with tempfile.TemporaryDirectory() as tmp:
            item = os.path.join(tmp, 'kernel'); target = os.path.join(tmp, 'payload')
            with open(item, 'wb') as f: f.write(b'abc')
            device.payload(target, [('kernel', item)], cmdline='-v', usb=True)
            with open(target, 'rb') as f: data = f.read()
            self.assertEqual(struct.unpack_from('<II', data), (0x50554e58, len(data) - 8))
            self.assertEqual(struct.unpack_from('<16sII', data, 8), (b'kernel' + b'\0' * 10, 3, 0))
            self.assertEqual(data[32:35], b'abc')
            self.assertEqual(data[-24:], b'\0' * 24)

    def test_png_channels_padding_and_scale(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, 's.png')
            raw = bytes([3, 2, 1, 255, 6, 5, 4, 255]) + b'padding!'
            device.write_png(target, raw * 2, 2, 2, 16, 1)
            with open(target, 'rb') as f: data = f.read()
            self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
            at = 8; compressed = b''
            while at < len(data):
                n = struct.unpack_from('>I', data, at)[0]; kind = data[at + 4:at + 8]
                if kind == b'IDAT': compressed += data[at + 8:at + 8 + n]
                at += n + 12
            self.assertEqual(zlib.decompress(compressed), b'\0\1\2\3\4\5\6' * 2)

    def test_screenshot_rejects_scale_before_usb(self):
        for scale in (0, -1):
            with self.assertRaises(Fail) as error:
                device.screenshot('unused.png', scale)
            self.assertEqual(error.exception.code, 'usage')

    def test_run_extra_args_not_discarded(self):
        result = {'summary': {'panics': 0, 'init': {'failed': 0, 'summary_seen': False}, 'marks': {'verdicts': []}}, 'result': 'returned'}
        with patch.object(device, 'run', return_value=result) as run, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.main(['run', '--bootargs', 'cpus=2', '--extra-bootargs', 'iphont=1']), 0)
            self.assertEqual(run.call_args.kwargs['bootargs'], 'cpus=2 iphont=1')
        self.assertFalse(result['boot_validated'])

    def test_false_verdict_fails_run(self):
        result = {'summary': {'panics': 0, 'init': {'failed': 0, 'summary_seen': True},
                             'marks': {'verdicts': [{'pass': False}]}}, 'result': 'returned'}
        with patch.object(device, 'run', return_value=result), patch.object(device, 'profile', return_value={}), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cli.main(['run']), 1)

    def test_log_filters_and_oslog_levels(self):
        recs = logs.lines('CoreData: debug: error handler\nadreno: failed\ntl: p 1 0 10 20 at 30\n')
        self.assertEqual(recs[0]['severity'], 'debug')
        self.assertEqual(len(list(logs.select(recs, component='tl'))), 1)
        self.assertEqual(len(list(logs.select(recs, min_severity='error'))), 1)

    def test_panic_and_crash_records(self):
        text = ('panic(cpu 1 caller 0xffff1234): failure\nKernel UUID: ABC\nKernel text base:  0xffff0000\n'
                '  lr: 0xffff2345  fp: 0xffff3456\n'
                'usertrap: app[22] exception 1 code 0x1 0x2: pc 0x123 lr 0x456 sp 0x789, shared cache slide 0x0\n'
                'usertrap:   called from 0x456\n')
        self.assertEqual(logs.panics(text)[0]['cpu'], 1)
        self.assertEqual(logs.crashes(text)[0]['frames'], ['0x456'])

    def test_rpc_packet_and_sequence(self):
        body = b'{"control":true}'
        data = agent.HEADER.pack(1, 25, 0, len(body), 12345) + body
        with patch.object(link, 'getvar', return_value='1'), patch.object(link, 'oem') as oem, patch.object(link, 'fetch', return_value=data), patch.object(link, 'transaction', contextlib.nullcontext), patch.object(agent.secrets, 'randbelow', return_value=24):
            r = agent.request('hello')
            self.assertTrue(r['data']['control']); self.assertEqual(r['device_ns'], 12345)
            self.assertEqual(oem.call_args_list[1].args, ('iphont-go', '19', '5'))

    def test_rpc_errors_and_malformed_packets(self):
        for data, code in [(b'x', 'no_answer'), (agent.HEADER.pack(2, 3, 0, 0, 0), 'no_answer'),
                           (agent.HEADER.pack(1, 3, 1, 0, 0), 'refused')]:
            with self.subTest(code=code), patch.object(link, 'getvar', return_value='1'), patch.object(link, 'oem'), patch.object(link, 'fetch', return_value=data), patch.object(link, 'transaction', contextlib.nullcontext), self.assertRaises(Fail) as error:
                agent.request('hello', sequence=3)
            self.assertEqual(error.exception.code, code)

    def test_sysctl_types_are_explicit(self):
        result = {'data': {'hex': '01000000', 'bytes': 4}}
        with patch.object(agent, 'request', return_value=result):
            self.assertEqual(agent.sysctl('unknown.name')['data']['value'], '01000000')
            self.assertEqual(agent.sysctl('hw.ncpu', 'uint32')['data']['value'], 1)

    def test_stream_offsets(self):
        for data, good in [(struct.pack('<QQQQ', 10, 13, 15, 4) + b'abc', True), (struct.pack('<QQQQ', 13, 10, 15, 0), False)]:
            with patch.object(link, 'transaction', contextlib.nullcontext), patch.object(link, 'getvar', return_value='1'), patch.object(link, 'oem'), patch.object(link, 'fetch', return_value=data):
                if good: self.assertEqual(stream.tail(6), (10, 13, 15, 4, b'abc'))
                else:
                    with self.assertRaises(Fail): stream.tail(0)

    def test_snapshot_does_not_fake_unsupported_agent(self):
        with patch.object(device, 'info', return_value={'state': 'loader'}), patch.object(agent, 'request', side_effect=Fail('old_loader', 'old')):
            s = diagnostics.snapshot()
        self.assertEqual(s['layers'], {})
        self.assertEqual(s['agent_error']['code'], 'old_loader')

    def test_critical_writes_require_authorization(self):
        with patch.object(link, 'transaction', contextlib.nullcontext), patch.object(link, 'fastboot') as boot:
            with self.assertRaises(Fail): device.install('nonexistent')
            with self.assertRaises(Fail): device.flashpart('boot_b', 'nonexistent', '0' * 64)
            boot.assert_not_called()

    def test_gesture_runs_once_on_device(self):
        with patch.object(agent, 'request', return_value={'data': {'events_queued': 10}}) as request:
            agent.gesture([(0, 0), (10, 20), (20, 40)], .3, 9)
            request.assert_called_once_with('gesture 0 0 20 40 300 2 9')

    def test_user_errors_propagate(self):
        with patch.object(agent, 'request', return_value={'data': {'ok': False, 'errno': 1}}), self.assertRaises(Fail):
            agent.user('key', usage=4)

    def test_rpc_rejects_invalid_sequence(self):
        for seq in (0, -1, 0x100000000):
            with self.assertRaises(Fail) as error:
                agent.request('hello', sequence=seq)
            self.assertEqual(error.exception.code, 'usage')

    def test_live_section_and_inferred_severity(self):
        records = logs.lines('== KERNEL LOG (the kernel is still running) ==\nadreno: error 0\n')
        self.assertEqual(records[1]['section'], 'kernel')
        self.assertEqual(records[1]['severity_source'], 'inferred')

    def test_unknown_kernel_uuid_does_not_crash(self):
        self.assertIsNone(symbols.kernel_symbolizer(path=None))

    def test_crash_attribution_is_not_function_symbolication(self):
        with patch.object(symbols, 'user_symbolizer', return_value=lambda _: {'image': 'libtest', 'offset': '0x10'}):
            record = symbols.symbolize_crash({'pc': '0x1000'})
            self.assertFalse(record['symbolized'])
            self.assertTrue(record['image_attribution'])

    def test_stop_cannot_end_same_checkout_peer(self):
        with patch.object(link, 'run_held', return_value=device.ROOT), patch.object(link, 'oem') as oem:
            with self.assertRaises(Fail): device.stop()
            oem.assert_not_called()

    def test_raw_log_preserves_bytes(self):
        raw = b'hello\0\xffworld'
        def fetch(path, **kw):
            with open(path, 'wb') as f: f.write(raw)
            return raw
        with tempfile.TemporaryDirectory() as tmp, patch.object(link, 'oem'), patch.object(link, 'fetch', side_effect=fetch), patch.object(link, 'transaction', contextlib.nullcontext):
            dest = os.path.join(tmp, 'console.log')
            self.assertIn('world', device.fetch_log(dest))
            with open(dest + '.raw', 'rb') as f: self.assertEqual(f.read(), raw)

    def test_kernel_test_missing_probe_is_failure(self):
        result = workflow.kernel_test_results('iphont-test: {"probe":"hello","errno":5,"result":null}\n')
        self.assertFalse(result['passed'])
        self.assertTrue(result['errors'])

    def test_kernel_test_parser_recovers_complete_replay(self):
        results = {
            'hello': {'control': False}, 'sysctl kern.osrelease': {'hex': '32322e352e3000'},
            'processes': {'processes': [{'pid': 0}]}, 'registry': {'entries': [{'id': 1}]},
            'threads 0': {'threads': [{'id': 1}]}, 'events 0': {'events': [{'seq': 1}]},
        }
        text = 'iphont-test: {incomplete\n'
        for name, result in results.items():
            text += 'iphont-test: ' + json.dumps({'probe': name, 'errno': 0, 'result': result}) + '\n'
        text += 'iphont-test: ' + json.dumps({'probe': 'control denied', 'errno': 1}) + '\n'
        text += 'iphont-test: done\n--- loader_return_after expired ---\n'
        report = workflow.kernel_test_results(text)
        self.assertTrue(report['passed'], report)
        self.assertEqual(report['incomplete_console_copies'], 1)

    def test_oversized_payload_rejected_before_read(self):
        with patch.object(os.path, 'getsize', return_value=100 * 1024 * 1024), self.assertRaises(Fail) as error:
            device.payload('unused.bin', [('kernel', 'not-opened')])
        self.assertEqual(error.exception.code, 'refused')

    def test_failed_capture_keeps_partial_manifest(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(device, 'screenshot', side_effect=Fail('unsupported', 'no display')):
            target = os.path.join(tmp, 'capture')
            with self.assertRaises(Fail): list(stream.frames(target, .01))
            with open(os.path.join(target, 'recording.json')) as f: manifest = json.load(f)
            self.assertFalse(manifest['complete'])
            self.assertEqual(manifest['frames'], [])

    def test_stream_reconnect_and_log_gap_are_visible(self):
        replies = [Fail('timeout', 'USB out'), (10, 12, 12, 10, b'x\n')]
        with patch.object(stream, 'tail', side_effect=replies), patch.object(link, 'getvar', return_value='returned'), patch.object(device, 'fetch_log', return_value='finished'), patch.object(stream.time, 'sleep'):
            records = list(stream.follow(seconds=2, interval=.01))
        events = [r['event'] for r in records]
        self.assertEqual(events[:3], ['connection_lost', 'connection_restored', 'log_gap'])
        self.assertEqual(events[-1], 'run_ended')

    def test_enter_refuses_wrong_stock_product(self):
        with patch.object(link, 'state', return_value='fastboot'), patch.object(device, 'profile', return_value={'FASTBOOT_PRODUCT': 'kona'}), patch.object(link, 'getvar', return_value='redwood'), self.assertRaises(Fail) as error:
            device.enter()
        self.assertEqual(error.exception.code, 'refused')

    def test_text_cleanup_preserves_uncertain_request(self):
        import argparse
        primary = Fail('agent_timeout', 'unknown outcome', detail={'sequence': 13})
        with patch.object(agent, 'user', side_effect=[primary, Fail('refused', 'pending')]), self.assertRaises(Fail) as error:
            cli.cmd_text(argparse.Namespace(text='a'))
        self.assertEqual(error.exception.code, 'agent_timeout')
        self.assertEqual(error.exception.detail['original']['sequence'], 13)
        self.assertEqual(error.exception.detail['release_errors'][0]['code'], 'refused')


if __name__ == '__main__': unittest.main()
