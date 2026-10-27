#!/usr/bin/env python3
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from summarize_network_benchmark import summarize


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.folder = Path(self.tmp.name)
        (self.folder/'source').mkdir()
        hashes = {}
        for name in ['network_benchmark.py','network_benchmark.html']:
            file = self.folder/'source'/name
            file.write_text('frozen test fixture')
            hashes[name] = hashlib.sha256(file.read_bytes()).hexdigest()
        self.data = dict(peer='192.168.31.64', complete=True, source_sha256=hashes,
            context=dict(image_sha256='image',expected_peer_for_phone_run='192.168.31.64'), stages=[])
        for direction in ['download','upload']:
            for index in range(4):
                self.data['stages'].append(dict(direction=direction, warmup=index==0, finished=True,
                    crc_errors=0,errors=0,mbps=1,bytes=7500000,elapsed_ms=60000,
                    rtt_ms=[1,2,3],ping_failures=0,ping_cancelled_at_stage_end=0))

    def tearDown(self):
        self.tmp.cleanup()

    def check(self):
        file = self.folder/'run.json'
        file.write_text(json.dumps(self.data))
        return summarize(file,'image')

    def test_only_complete_three_windows_are_baseline(self):
        self.assertTrue(self.check()['complete_three_by_sixty_second_baseline'])
        self.data['stages']=[self.data['stages'][i] for i in [0,1,4,5]]
        result=self.check()
        self.assertTrue(result['complete_single_run'])
        self.assertFalse(result['complete_three_by_sixty_second_baseline'])

    def test_partial_corrupt_or_short_never_passes(self):
        for mode in ['partial','corrupt','short']:
            original=copy.deepcopy(self.data)
            if mode=='partial':self.data['complete']=False
            if mode=='corrupt':self.data['stages'][2]['crc_errors']=1
            if mode=='short':self.data['stages'][2]['elapsed_ms']=59999
            self.assertFalse(self.check()['complete_three_by_sixty_second_baseline'])
            self.data=original

    def test_identity_peer_and_frozen_source_are_required(self):
        original=copy.deepcopy(self.data)
        for key,value in [('peer','127.0.0.1'),('context',dict(image_sha256='other'))]:
            self.data[key]=value
            with self.assertRaises(ValueError):self.check()
            self.data=copy.deepcopy(original)
        (self.folder/'source/network_benchmark.py').write_text('changed')
        with self.assertRaises(ValueError):self.check()

    def test_download_goal_requires_every_sixty_second_window(self):
        self.data['directions']=['download']
        self.data['stages']=self.data['stages'][:4]
        for stage in self.data['stages']:
            stage['mbps']=5.2;stage['bytes']=39000000
        self.assertTrue(self.check()['download_5mbps_each_window_met'])
        self.data['stages'][2]['mbps']=4.9
        self.data['stages'][2]['bytes']=36750000
        result=self.check()
        self.assertTrue(result['complete_three_by_sixty_second_baseline'])
        self.assertFalse(result['download_5mbps_each_window_met'])

    def test_rate_must_match_received_bytes_and_elapsed_time(self):
        self.data['stages'][2]['mbps']=50
        result=self.check()
        self.assertFalse(result['complete_three_by_sixty_second_baseline'])
        self.assertFalse(result['download_5mbps_each_window_met'])


if __name__=='__main__':unittest.main(verbosity=2)
