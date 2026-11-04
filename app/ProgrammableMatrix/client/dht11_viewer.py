"""Minimal display for an uploaded DHT11 program on a selected JBOX device."""
from __future__ import annotations
import argparse
from pathlib import Path
import queue
import threading
import time
import tkinter as tk

try:
    from .jbox_api import MatrixClient, APIError, discover_devices, pair_device
except ImportError:
    from jbox_api import MatrixClient, APIError, discover_devices, pair_device

PROGRAM = Path(__file__).with_name('programs') / 'dht11.jbox'
# CN1: 39/40 VCC, 37/38 DATA, 35/36 NC, 33/34 GND.
# VCC uses 5V through the matrix; DATA retains GPIO1's internal 3.3V pull-up.
DHT_ROUTES = [[19, 49], [18, 40], [16, 51]]


def configure_sensor(board, source=None):
    # Upload first: old firmware is rejected before any sensor power is routed.
    read_sensor = board.upload_function(source or PROGRAM.read_text(encoding='utf-8'))
    board.gpio_config(0, mode=2, pull=1, initial=1)
    board.route_set(DHT_ROUTES)
    return read_sensor


def read_values(read_sensor):
    values = read_sensor(data=0)
    if len(values) != 2 or not 0 <= values[0] <= 500 or not 0 <= values[1] <= 1000:
        raise ValueError('传感器返回值无效')
    return values[0] / 10, values[1] / 10


def error_text(error):
    if isinstance(error, APIError):
        return {-5: '请先烧录支持 JBoxScript 的新固件',
                -6: '传感器无响应：检查实际供电与连线',
                -3: '传感器数据校验失败，正在重试',
                -7: '请短按 GPIO10 后重新连接',
                -10: '设备上的函数已清除，请重新连接上传'}.get(error.status, str(error))
    if 'paired' in str(error):
        return '请短按 GPIO10 后重新连接，完成电脑绑定'
    return f'连接失败：{error}'


class DHTWindow(tk.Tk):
    def __init__(self, host):
        super().__init__()
        self.tk.call('tk', 'scaling', 96 / 72)
        self.title('DHT11')
        self.geometry('430x285')
        self.resizable(False, False)
        self.configure(bg='#111b29')
        self.host = tk.StringVar(value=host)
        self.temperature = tk.StringVar(value='— °C')
        self.humidity = tk.StringVar(value='— %')
        self.status = tk.StringVar(value='CN1：39 5V · 37 DATA · 35 NC · 33 GND')
        self.events = queue.Queue()
        self.stop_event = None
        self.thread = None
        row = tk.Frame(self, bg='#111b29')
        row.pack(fill='x', padx=20, pady=(18, 12))
        tk.Entry(row, textvariable=self.host, font=('Segoe UI', 12), bg='#223246', fg='white',
                 insertbackground='white', relief='flat', width=22).pack(side='left', ipady=6)
        self.connect = tk.Button(row, text='连接', command=self.toggle, bg='#58c4ff',
                                 fg='#102030', relief='flat', padx=18, pady=7)
        self.connect.pack(side='right')
        values = tk.Frame(self, bg='#111b29')
        values.pack(fill='x', padx=20, pady=8)
        for column, (label, variable) in enumerate((('温度', self.temperature), ('湿度', self.humidity))):
            card = tk.Frame(values, bg='#1b2a3c', padx=14, pady=15)
            card.grid(row=0, column=column, sticky='ew', padx=(0, 5) if column == 0 else (5, 0))
            values.columnconfigure(column, weight=1)
            tk.Label(card, text=label, bg='#1b2a3c', fg='#94a9c1', font=('Microsoft YaHei UI', 10)).pack(anchor='w')
            tk.Label(card, textvariable=variable, bg='#1b2a3c', fg='#e8f3ff', font=('Segoe UI', 30)).pack(anchor='w')
        tk.Label(self, textvariable=self.status, bg='#111b29', fg='#9fb5cc',
                 font=('Microsoft YaHei UI', 9), wraplength=390, justify='left').pack(anchor='w', padx=20, pady=10)
        self.after(100, self.receive)
        self.protocol('WM_DELETE_WINDOW', self.close_window)

    def toggle(self):
        if self.thread and self.thread.is_alive():
            self.stop_event.set()
            self.connect.configure(state='disabled')
            self.status.set('正在断开…')
            return
        host = self.host.get().strip()
        self.temperature.set('— °C'); self.humidity.set('— %')
        self.status.set('正在连接并上传程序…')
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.run_sensor, args=(host, self.stop_event), daemon=True)
        self.thread.start()
        self.connect.configure(text='断开')

    def run_sensor(self, host, stop):
        board = None
        try:
            devices = discover_devices(timeout=0.8)
            if not host:
                if len(devices) != 1:
                    raise ValueError('请填写设备 IP，或确保局域网只发现一台设备')
                host = devices[0]['host']
            self.events.put(('host', host))
            device = next((d for d in devices if d['host'] == host), {})
            if device.get('pairing_open'):
                pair_device(host, device.get('id'))
            board = MatrixClient(host, timeout=10)
            board.connect()
            read_sensor = configure_sensor(board)
            self.events.put(('status', '程序已上传，等待传感器上电稳定…'))
            if stop.wait(2):
                return
            while not stop.is_set():
                started = time.monotonic()
                try:
                    temperature, humidity = read_values(read_sensor)
                    self.events.put(('values', (temperature, humidity)))
                except APIError as error:
                    if error.status not in (-6, -3):
                        raise
                    # The VM releases GPIOs to floating inputs on errors. Restore
                    # the DHT bus idle level before waiting for the next attempt.
                    board.gpio_config(0, mode=2, pull=1, initial=1)
                    self.events.put(('sensor_error', error_text(error)))
                if stop.wait(max(0, 2.5 - (time.monotonic() - started))):
                    break
        except Exception as error:
            self.events.put(('sensor_error', error_text(error)))
        finally:
            if board:
                board.close()
            self.events.put(('done', None))

    def receive(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == 'values':
                    temperature, humidity = value
                    self.temperature.set(f'{temperature:g} °C')
                    self.humidity.set(f'{humidity:g} %')
                    self.status.set(f'实时读取 · {time.strftime("%H:%M:%S")} · 每 2.5 秒更新')
                elif kind == 'sensor_error':
                    # Never present old data as a fresh successful measurement.
                    self.temperature.set('— °C'); self.humidity.set('— %')
                    self.status.set(value)
                elif kind == 'host':
                    self.host.set(value)
                elif kind == 'status':
                    self.status.set(value)
                elif kind == 'done':
                    self.thread = None
                    self.connect.configure(text='连接', state='normal')
                    if self.stop_event and self.stop_event.is_set():
                        self.temperature.set('— °C'); self.humidity.set('— %')
                        self.status.set('已断开，传感器供电与数据路由已释放')
        except queue.Empty:
            pass
        self.after(100, self.receive)

    def close_window(self):
        if self.stop_event:
            self.stop_event.set()
        for callback in self.tk.splitlist(self.tk.call('after', 'info')):
            self.after_cancel(callback)
        self.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='', help='设备 IP；留空自动发现唯一设备')
    parser.add_argument('--connect', action='store_true', help='打开窗口后连接并上传小程序')
    args = parser.parse_args()
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    window = DHTWindow(args.host)
    if args.connect:
        window.after(250, window.toggle)
    window.mainloop()


if __name__ == '__main__':
    main()
