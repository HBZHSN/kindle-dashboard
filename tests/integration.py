"""Loopback protocol check against the actual C++ executable; no external API calls."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from datetime import datetime, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[1]
os.environ['TZ'] = 'Asia/Shanghai'
time.tzset()
fail = False
hits = set()
errors = []
custom_products = {'TEST': 'Demo Product', 'TEST_NULL': 'null', 'TEST_MISSING': 'missing', 'TEST_ZERO': 'zero'}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = urlparse(self.path).path
        hits.add(path)
        if fail:
            self.send_response(503)
            self.end_headers()
            return
        try:
            now = datetime.now()
            if path == '/api/history':
                query = parse_qs(urlparse(self.path).query)
                product = query.get('product', [''])[0]
                assert product in custom_products.values()
                assert query == {'date': [now.strftime('%Y-%m-%d')], 'product': [product]}
                assert 'Authorization' not in self.headers
                stamp = int(now.replace(hour=0, minute=0, second=0, microsecond=0).timestamp() * 1000)
                result = [{'time': stamp, 'ret': .01, 'exposure': .7},
                          {'time': stamp + 1000, 'ret': .0123, 'exposure': .74}]
                if product == 'null':
                    result[-1]['exposure'] = None
                elif product == 'missing':
                    del result[-1]['exposure']
                elif product == 'zero':
                    result[-1]['exposure'] = 0
            elif path == '/forecast':
                result = {'current': {'temperature_2m': 28.6, 'relative_humidity_2m': 65, 'weather_code': 2},
                          'daily': {'temperature_2m_min': [23.4], 'temperature_2m_max': [31.2]},
                          'hourly': {'precipitation_probability': [20], 'precipitation': [.1], 'uv_index': [3.2]}}
            elif path == '/air':
                result = {'current': {'us_aqi': 42}}
            elif path in ('/cloud', '/cloud-air'):
                start = ((int(time.time()) + 8 * 3600 + 10799) // 10800) * 10800 - 8 * 3600
                stamps = [start + i * 10800 for i in range(12)]
                if path == '/cloud-air':
                    result = {'hourly': {'time': stamps, 'aerosol_optical_depth': [.35] * 12}}
                else:
                    result = {'properties': {'timeseries': [
                        {'time': datetime.fromtimestamp(stamp).isoformat(),
                         'data': {'instant': {'details': {'cloud_area_fraction': 40, 'air_temperature': 20}},
                                  'next_6_hours': {'details': {'precipitation_amount': 1.2}}}}
                        for stamp in stamps]}}
            elif path == '/sunset':
                code = parse_qs(urlparse(self.path).query)['event'][0]
                day = now + timedelta(days=int(code[-1]) - 1)
                result = {'status': 'ok', 'local_aod': .21, 'local_quality': .42,
                          'tb_event_time': day.strftime('%Y-%m-%d') + ('T05:48:00+08:00' if code.startswith('rise') else 'T17:57:00+08:00')}
            elif path == '/api/usage':
                assert self.headers['Authorization'] == 'Bearer usage-test'
                window = {'used_percent': 35, 'reset_after_human': '2h 36m'}
                result = {'codex': {'primary_window': window, 'secondary_window': window},
                          'agy': {'gemini_5h': window, 'gemini_weekly': window}}
            elif path == '/news':
                assert parse_qs(urlparse(self.path).query) == {'platform': ['eastmoney']}
                result = {'status': '200', 'data': [
                    {'title': '测试财经新闻', 'publish_time': '2026-09-23 17:08:54'},
                    {'title': 'Second item', 'publish_time': '2026-09-23 17:07:39'}]}
            elif path == '/tencent':
                fields = [''] * 31
                fields[3], fields[4], fields[30] = '101', '100', now.strftime('%Y%m%d%H%M%S')
                result = {'code': 0, 'data': {'sh000001': {'qt': {'sh000001': fields}, 'data': {'data': ['0930 100', '0931 101']}}}}
            elif path == '/futures':
                result = {'minLine_1d': [['09:30', '201', 0, 0, 0, '200', now.isoformat()]]}
            elif path == '/us':
                result = 'callback=("' + '09:30,0,0,100;' * 1000 + '09:31,0,0,102");'
            elif path == '/eastmoney':
                result = {'data': {'preClose': 100, 'trends': [now.strftime('%Y-%m-%d %H:%M') + ',0,103']}}
            elif path == '/ticker':
                result = {'openPrice': '100', 'lastPrice': '105', 'priceChangePercent': '5'}
            elif path == '/klines':
                result = [[0, 0, 0, 0, '105', 0, int(time.time() * 1000)]]
            else:
                raise AssertionError(path)
            body = (result if isinstance(result, str) else json.dumps(result)).encode()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(body)
        except Exception as exc:
            errors.append(repr(exc))
            self.send_response(500)
            self.end_headers()


server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_port}'
try:
    with tempfile.TemporaryDirectory(prefix='kindle-protocol-') as temp:
        folder = Path(temp)
        config = {'market': {'source': 'mix', 'mix_symbols': list(custom_products) + ['sh000001', 'GC', '.NDX', 'EM', 'BTC-USD'],
                            'custom_symbols': {symbol: {'url': base + '/api/history', 'product': product, 'sessions': ['00:00-23:59'], 'lookback_days': 0}
                                               for symbol, product in custom_products.items()},
                            'mix_sources': {symbol: {'provider': provider, 'url': base + '/' + route}
                                            for symbol, provider, route in [('sh000001', 'tencent', 'tencent'), ('GC', 'sina_futures', 'futures'), ('.NDX', 'sina_us', 'us'), ('EM', 'eastmoney', 'eastmoney')]},
                            'binance': {'ticker_url': base + '/ticker', 'klines_url': base + '/klines'}},
                  'weather': {'location': 'Hangzhou', 'latitude': 30.2, 'longitude': 120.2, 'forecast_url': base + '/forecast', 'air_quality_url': base + '/air'},
                  'cloud': {'url': 'http://forecast.invalid/cloud', 'air_quality_url': 'http://forecast.invalid/cloud-air', 'proxy': base},
                  'sunset': {'url': base + '/sunset'},
                  'agent_usage': {'url': base, 'password': 'usage-test'},
                  'news': {'url': base + '/news', 'platform': 'eastmoney'},
                  'refresh': {'poll_seconds': .1}, 'cache': {'data_path': 'snapshot.json'}}
        path = folder / 'config.yaml'
        path.write_text(json.dumps(config))
        command = [str(ROOT / '.build/native/kindle-dashboard'), '--config', str(path), '--assets', str(ROOT / 'assets'),
                   '--simulate', '--once', '--freeze-time', '--output', str(folder / 'preview')]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        assert not errors, errors
        data = json.loads((folder / 'snapshot.json').read_text())
        assert [q['price'] for q in data['market']] == ['74%', '', '', '0%', '101.00', '201.00', '102.00', '103.00', '105.00'], data['market']
        assert all(q['status'] == 'OK' for q in data['market'])
        for quote in data['market'][:len(custom_products)]:
            assert quote['change'] == '+1.23%'
            assert quote['intraday'][0] == 1.23
        assert data['weather']['aqi'] == 'AQI 42'
        assert data['cloud']['status'] == 'OK' and len(data['cloud']['points']) == 12
        assert all(point['aod'] == .35 for point in data['cloud']['points'])
        assert data['agent_usage']['codex_5h']['used_percent'] == 35
        assert data['news']['items'][0]['title'] == '测试财经新闻'
        assert data['news']['platform'] == 'eastmoney'
        assert len(data['sunset']['events']) == 2
        assert {'/api/history', '/forecast', '/air', '/cloud', '/cloud-air', '/sunset', '/api/usage', '/news', '/tencent', '/futures', '/us', '/eastmoney', '/ticker', '/klines'} <= hits
        direct = dict(config, cloud={'url': base + '/cloud', 'air_quality_url': base + '/cloud-air'})
        # Use a fresh snapshot so the cloud cache cannot bypass the direct request.
        direct['cache'] = {'data_path': 'direct-snapshot.json'}
        path.write_text(json.dumps(direct))
        hits.clear()
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        assert {'/cloud', '/cloud-air'} <= hits, hits
        assert json.loads((folder / 'direct-snapshot.json').read_text())['cloud']['status'] == 'OK'
        path.write_text(json.dumps(config))
        # Expire the cloud cache so the failure case actually requests it again.
        data['cloud']['fetched_at'] = int(time.time()) - 10801
        (folder / 'snapshot.json').write_text(json.dumps(data))
        fail = True
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        after = json.loads((folder / 'snapshot.json').read_text())
        for region in ('market', 'cloud', 'weather', 'agent_usage', 'news'):
            assert after[region] == data[region], f'Network failure changed {region}'
        assert after['sunset']['events'] == data['sunset']['events']
        print('PASS: six market protocols, null/missing/zero exposure, cloud proxy/direct requests, weather, sunset, usage, failed requests preserve cached content')
finally:
    server.shutdown()
