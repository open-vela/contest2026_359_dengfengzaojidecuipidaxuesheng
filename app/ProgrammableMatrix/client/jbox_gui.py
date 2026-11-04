"""Local desktop controller. Run: python client/jbox_gui.py"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import queue
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    from . import jbox_api as api
    from .board_layout import DIRECT_HEADER, GPIO_LIST, NET_COLORS, RouteDesign, connector_rows, node_label
except ImportError:
    import jbox_api as api
    from board_layout import DIRECT_HEADER, GPIO_LIST, NET_COLORS, RouteDesign, connector_rows, node_label

BG = '#101722'
PANEL = '#182332'
BOARD = '#102f51'
INK = '#e5edf8'
MUTED = '#92a6bd'
ACCENT = '#58c4ff'


class DeviceWorker:
    """A single owner for the persistent TCP socket; never touches Tk widgets."""
    def __init__(self, client_factory=api.MatrixClient):
        self.client_factory = client_factory
        self.client = None
        self.jobs = queue.Queue()
        self.results = queue.Queue()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def submit(self, action, **args):
        self.jobs.put((action, args))

    def _snapshot(self):
        return {'status': self.client.status(),
                'levels': self.client.gpio_read(list(range(9, 18)))}

    def _execute(self, action, args):
        if action == 'discover':
            return api.discover_devices()
        if action == 'disconnect':
            if self.client:
                self.client.close()
                self.client = None
            return None
        if action == 'connect':
            host = args['host']
            devices = api.discover_devices(timeout=0.8)
            if not host:
                if len(devices) != 1:
                    raise ValueError('请搜索设备并选择一台，或输入设备 IP')
                host = devices[0]['host']
            device = next((d for d in devices if d['host'] == host), {})
            if args.get('pair') or device.get('pairing_open'):
                api.pair_device(host, device.get('id'))
            if self.client:
                self.client.close()
                self.client = None
            client = self.client_factory(host)
            try:
                client.connect()
                caps = client.caps()
                if caps.get('nodes') != 52 or caps.get('io_gpios') != list(GPIO_LIST):
                    raise ValueError('设备端子映射与此工具不兼容')
                self.client = client
                return dict(host=host, **self._snapshot())
            except Exception:
                client.close()
                self.client = None
                raise
        if not self.client:
            raise ConnectionError('设备尚未连接')
        if action == 'apply':
            self.client.route_set(args['nets'])
        elif action == 'safe':
            self.client.safe()
        elif action == 'gpio':
            self.client.gpio_config(args['io'], mode=args['mode'], initial=args['level'])
        elif action != 'poll':
            raise ValueError('未知操作')
        return self._snapshot()

    def _run(self):
        while True:
            job = self.jobs.get()
            if job is None:
                self._execute('disconnect', {})
                return
            action, args = job
            try:
                value = self._execute(action, args)
                self.results.put((action, value, None))
            except Exception as error:
                # Firmware parameter errors leave the authenticated session usable.
                if not isinstance(error, (api.APIError, ValueError)):
                    self._execute('disconnect', {})
                self.results.put((action, None, error))

    def stop(self):
        self.jobs.put(None)


class MatrixApp(tk.Tk):
    def __init__(self, host='', worker=None):
        super().__init__()
        # Canvas geometry and fonts use the same pixel scale, including on
        # Windows desktops with a 200% system font scale.
        self.tk.call('tk', 'scaling', 96 / 72)
        self.title('JBOX Studio · 可编程连线板')
        self.geometry('1420x940')
        self.minsize(1220, 850)
        self.configure(bg=BG)
        self.option_add('*Font', ('Microsoft YaHei UI', 10))
        self.worker = worker or DeviceWorker()
        self.design = RouteDesign()
        self.applied = [[] for _ in range(8)]
        self.connected = False
        self.busy = False
        self.submitted_nets = None
        self.closed = False
        self.levels = {}
        self.selected_gpio = None
        self.pin_positions = {}
        self.device_hosts = {}
        self.host = tk.StringVar(value=host)
        self.notice = tk.StringVar(value='先选一组网络，再点选端子；点击“应用连线”写入设备。')
        self.connection = tk.StringVar(value='离线设计')
        self.gpio_name = tk.StringVar(value='点击小排针上的 GPIO')
        self.gpio_mode = tk.StringVar(value='输入 / 高阻')
        self.gpio_level = tk.IntVar(value=0)
        self._style()
        self._build()
        self.protocol('WM_DELETE_WINDOW', self.close_app)
        self.after(80, self._receive)
        self.after(2000, self._poll)

    def _style(self):
        style = ttk.Style(self)
        style.theme_use('clam')
        style.configure('TCombobox', fieldbackground='#223246', background='#293c52', foreground=INK,
                        arrowcolor=INK, borderwidth=0, padding=7)
        style.map('TCombobox', fieldbackground=[('readonly', '#223246')],
                  foreground=[('readonly', INK)])

    @staticmethod
    def label(parent, text='', size=10, color=INK, **kwargs):
        return tk.Label(parent, text=text, font=('Microsoft YaHei UI', size),
                        fg=color, bg=parent.cget('bg'), **kwargs)

    @staticmethod
    def button(parent, text, command, primary=False, **kwargs):
        return tk.Button(parent, text=text, command=command, relief='flat', bd=0,
                         padx=15, pady=9, cursor='hand2',
                         bg=ACCENT if primary else '#28394d', fg=BG if primary else INK,
                         activebackground='#81d5ff' if primary else '#354e69',
                         activeforeground=BG if primary else INK,
                         disabledforeground='#63768b', **kwargs)

    def _build(self):
        header = tk.Frame(self, bg=BG)
        header.pack(fill='x', padx=28, pady=(20, 16))
        title = tk.Frame(header, bg=BG)
        title.pack(side='left')
        self.label(title, 'JBOX  /  STUDIO', 21).pack(anchor='w')
        self.label(title, '可编程连线板 · ESP32-S3 + CH446Q', color=MUTED).pack(anchor='w', pady=(4, 0))
        self.badge = self.label(header, size=11, color=ACCENT, textvariable=self.connection)
        self.badge.pack(side='right')

        toolbar = tk.Frame(self, bg=PANEL, padx=14, pady=12)
        toolbar.pack(fill='x', padx=28)
        self.label(toolbar, '设备 IP', color=MUTED).pack(side='left', padx=(0, 10))
        self.host_box = ttk.Combobox(toolbar, textvariable=self.host, width=22)
        self.host_box.pack(side='left', padx=(0, 8))
        self.host_box.bind('<<ComboboxSelected>>', self._choose_device)
        self.search_button = self.button(toolbar, '搜索设备', lambda: self._submit('discover'))
        self.search_button.pack(side='left', padx=4)
        self.connect_button = self.button(toolbar, '连接', self.connect_device, primary=True)
        self.connect_button.pack(side='left', padx=4)
        self.pair_button = self.button(toolbar, '按钮授权绑定', lambda: self.connect_device(pair=True))
        self.pair_button.pack(side='left', padx=4)
        self.apply_button = self.button(toolbar, '应用连线', self.apply_design, primary=True)
        self.apply_button.pack(side='right', padx=(8, 0))
        self.safe_button = self.button(toolbar, '全部断开 / 高阻', lambda: self._submit('safe'))
        self.safe_button.pack(side='right')

        body = tk.Frame(self, bg=BG)
        body.pack(fill='both', expand=True, padx=28, pady=18)
        left = tk.Frame(body, bg=PANEL)
        left.pack(side='left', fill='both', expand=True, padx=(0, 18))
        board_header = tk.Frame(left, bg=PANEL, padx=18, pady=12)
        board_header.pack(fill='x')
        self.label(board_header, '端子视图', 13).pack(side='left')
        self.label(board_header, '按照片方向 · 左端为 39/40 · 同列奇偶脚直通', 9, MUTED).pack(side='right')
        board_area = tk.Frame(left, bg=PANEL)
        board_area.pack(fill='both', expand=True, padx=12)
        self.canvas = tk.Canvas(board_area, bg=BOARD, highlightthickness=0)
        board_bar = ttk.Scrollbar(board_area, orient='vertical', command=self.canvas.yview)
        board_bar.pack(side='right', fill='y')
        self.canvas.pack(side='left', fill='both', expand=True)
        self.canvas.configure(yscrollcommand=board_bar.set)
        self.canvas.bind('<Configure>', lambda event: self.draw_board())
        self.canvas.bind('<Button-1>', self._canvas_click)
        self.canvas.bind('<Motion>', self._hover)
        self.tooltip = tk.StringVar(value='CN1/CN2 的上下两孔对应同一个物理节点。')
        self.label(left, color=MUTED, size=9, textvariable=self.tooltip, anchor='w').pack(fill='x', padx=18, pady=10)

        side_container = tk.Frame(body, bg=BG, width=296)
        side_container.pack(side='right', fill='y')
        side_container.pack_propagate(False)
        side_scroll = tk.Canvas(side_container, bg=BG, bd=0, highlightthickness=0)
        side_bar = ttk.Scrollbar(side_container, orient='vertical', command=side_scroll.yview)
        side_bar.pack(side='right', fill='y')
        side_scroll.pack(side='left', fill='both', expand=True)
        side_scroll.configure(yscrollcommand=side_bar.set)
        side = tk.Frame(side_scroll, bg=BG)
        side_window = side_scroll.create_window((0, 0), window=side, anchor='nw')
        side.bind('<Configure>', lambda event: side_scroll.configure(scrollregion=side_scroll.bbox('all')))
        side_scroll.bind('<Configure>', lambda event: side_scroll.itemconfigure(side_window, width=event.width))
        self.label(side, '网络分组', 14).pack(anchor='w')
        self.label(side, '选组 → 点端子 → 应用', 9, MUTED).pack(anchor='w', pady=(3, 10))
        self.net_buttons = []
        for index, color in enumerate(NET_COLORS):
            btn = tk.Button(side, anchor='w', bd=0, relief='flat', padx=12, pady=8,
                            fg=color, bg=PANEL, activebackground='#2c4057', activeforeground=color,
                            cursor='hand2', command=lambda i=index: self.select_group(i))
            btn.pack(fill='x', pady=3)
            self.net_buttons.append(btn)
        self.members = tk.Text(side, height=3, bg=PANEL, fg=INK, bd=0, wrap='word',
                               font=('Microsoft YaHei UI', 9), padx=10, pady=8, state='disabled')
        self.members.pack(fill='x', pady=(8, 6))
        self.button(side, '清空当前组', self.clear_group).pack(fill='x')

        files = tk.Frame(side, bg=BG)
        files.pack(fill='x', pady=(12, 0))
        self.button(files, '保存方案', self.save_design).pack(side='left', fill='x', expand=True, padx=(0, 4))
        self.button(files, '打开方案', self.open_design).pack(side='left', fill='x', expand=True)
        self.label(side, '直出 GPIO', 13).pack(anchor='w', pady=(16, 5))
        self.label(side, textvariable=self.gpio_name, size=9, color=MUTED).pack(anchor='w')
        ttk.Combobox(side, textvariable=self.gpio_mode, state='readonly',
                     values=('输入 / 高阻', '推挽输出', '开漏输出')).pack(fill='x', pady=7)
        level_row = tk.Frame(side, bg=BG)
        level_row.pack(fill='x')
        for level in (0, 1):
            tk.Radiobutton(level_row, text=f'{level} 电平', value=level, variable=self.gpio_level,
                           bg=BG, fg=INK, selectcolor=PANEL, activebackground=BG,
                           activeforeground=INK).pack(side='left')
        self.gpio_button = self.button(side, '设置此 GPIO', self.configure_gpio)
        self.gpio_button.pack(fill='x', pady=(4, 0))

        footer = tk.Frame(self, bg=PANEL, padx=18, pady=12)
        footer.pack(fill='x', padx=28, pady=(0, 18))
        self.label(footer, textvariable=self.notice, color=INK, anchor='w', wraplength=1150).pack(fill='x')
        self.label(footer, '关闭窗口或网络断开后，设备自动清空矩阵连线并恢复 GPIO 高阻。', 9, MUTED).pack(anchor='w', pady=(5, 0))
        self.refresh()

    def _choose_device(self, event=None):
        value = self.host.get()
        self.host.set(self.device_hosts.get(value, value))

    def _submit(self, action, **args):
        if self.busy:
            return
        self.busy = True
        if action == 'apply':
            self.submitted_nets = [net[:] for net in args['nets']]
        self.worker.submit(action, **args)
        self.refresh_controls()
        if action != 'poll':
            self.notice.set({'discover': '正在搜索局域网设备…', 'connect': '正在连接和鉴权…',
                             'disconnect': '正在断开连接…', 'apply': '正在应用连线…',
                             'safe': '正在断开全部连线…', 'gpio': '正在设置 GPIO…'}.get(action, '处理中…'))

    def connect_device(self, pair=False):
        if self.connected and not pair:
            self._submit('disconnect')
        else:
            if pair:
                self.notice.set('请先短按并松开 GPIO10，再进行绑定。')
            self._submit('connect', host=self.host.get().strip(), pair=pair)

    def _receive(self):
        if self.closed:
            return
        try:
            while True:
                action, value, error = self.worker.results.get_nowait()
                self.busy = False
                if error:
                    if self.worker.client is None:
                        self.connected = False
                        self.applied = [[] for _ in range(8)]
                        self.levels = {}
                    self.notice.set(self._error_text(error))
                    if action != 'poll':
                        messagebox.showerror('操作未完成', self._error_text(error), parent=self)
                elif action == 'discover':
                    self.device_hosts = {f"{d['host']} · {d['id']}": d['host'] for d in value}
                    self.host_box['values'] = list(self.device_hosts)
                    if len(value) == 1:
                        self.host.set(value[0]['host'])
                    self.notice.set(f'发现 {len(value)} 台设备。首次绑定请先短按 GPIO10。')
                elif action == 'disconnect':
                    self.connected = False
                    self.applied = [[] for _ in range(8)]
                    self.levels = {}
                    self.notice.set('已断开连接，设备连线已释放；本地设计保留。')
                else:
                    if action == 'connect':
                        self.connected = True
                        self.host.set(value['host'])
                        if not any(self.design.nets):
                            self.design.load(value['status']['nets'])
                        self.notice.set('已连接。点选端子后点击“应用连线”。')
                    self.applied = value['status']['nets']
                    self.levels = value['levels']
                    if action in ('apply', 'safe'):
                        if action == 'safe' or self.design.nets == self.submitted_nets:
                            self.design.load(self.applied)
                        if action == 'safe':
                            self.notice.set('全部矩阵连线已断开，GPIO 已恢复高阻。')
                        elif self.design.nets == self.applied:
                            self.notice.set('连线已应用并读取确认。')
                        else:
                            self.notice.set('提交的连线已应用；之后编辑的更改仍待应用。')
                    elif action == 'gpio':
                        self.notice.set('GPIO 配置已发送，电平已读取。')
                self.refresh()
        except queue.Empty:
            pass
        self.after(80, self._receive)

    @staticmethod
    def _error_text(error):
        if isinstance(error, api.APIError):
            return {-7: '授权失败：短按 GPIO10 后点击“按钮授权绑定”。',
                    -4: '固件拒绝该连线，请检查电源与 EXT 的连接。',
                    -2: 'GPIO 正被外设占用，请先释放外设。'}.get(error.status, str(error))
        if isinstance(error, ValueError):
            if 'paired' in str(error):
                return '电脑尚未绑定：短按并松开 GPIO10，再点击“按钮授权绑定”。'
            return str(error)
        return f'连接或通信失败：{error}。请检查设备 IP 和同一局域网连接。'

    def _poll(self):
        if self.closed:
            return
        if self.connected and not self.busy:
            self._submit('poll')
        self.after(2000, self._poll)

    def refresh_controls(self):
        idle = not self.busy
        for widget in (self.connect_button, self.search_button, self.pair_button):
            widget.configure(state='normal' if idle else 'disabled')
        self.connect_button.configure(text='断开' if self.connected else '连接')
        for widget in (self.apply_button, self.safe_button):
            widget.configure(state='normal' if self.connected and idle else 'disabled')
        self.gpio_button.configure(state='normal' if self.connected and idle and self.selected_gpio is not None else 'disabled')

    def refresh(self):
        if self.selected_gpio is not None:
            level = self.levels.get(GPIO_LIST.index(self.selected_gpio), '—')
            self.gpio_name.set(f'GPIO{self.selected_gpio} · 电平 {level}')
        dirty = self.design.nets != self.applied
        self.connection.set(('● 已连接 · ' + ('有待应用更改' if dirty else '已同步')) if self.connected else '○ 离线设计')
        self.badge.configure(fg='#f5b85b' if self.connected and dirty else ACCENT)
        for index, btn in enumerate(self.net_buttons):
            active = index == self.design.active
            btn.configure(text=f"{'●' if active else '○'}   NET {index + 1}                      {len(self.design.nets[index])} 节点",
                          bg='#2a3b50' if active else PANEL)
        self.members.configure(state='normal')
        self.members.delete('1.0', 'end')
        self.members.insert('1.0', '  ·  '.join(node_label(n) for n in self.design.nets[self.design.active]) or '当前组为空，点击端子加入。')
        self.members.configure(state='disabled')
        self.refresh_controls()
        self.draw_board()

    def select_group(self, group):
        self.design.active = group
        self.refresh()

    def select_node(self, node):
        try:
            self.design.toggle(node)
        except ValueError as error:
            self.notice.set(str(error))
            return
        self.notice.set(f'已编辑 NET {self.design.active + 1}：{node_label(node)}。点击“应用连线”生效。')
        self.refresh()

    def clear_group(self):
        self.design.nets[self.design.active] = []
        self.notice.set('当前组已清空；点击“应用连线”写入设备。')
        self.refresh()

    def apply_design(self):
        self._submit('apply', nets=[list(net) for net in self.design.nets])

    def configure_gpio(self):
        if self.selected_gpio is None:
            return
        mode = ('输入 / 高阻', '推挽输出', '开漏输出').index(self.gpio_mode.get())
        self._submit('gpio', io=GPIO_LIST.index(self.selected_gpio), mode=mode, level=self.gpio_level.get())

    def save_design(self):
        filename = filedialog.asksaveasfilename(parent=self, title='保存连线方案',
                                              defaultextension='.json', filetypes=[('连线方案', '*.json')])
        if filename:
            try:
                Path(filename).write_text(json.dumps(self.design.export(), ensure_ascii=False, indent=2), encoding='utf-8')
                self.notice.set('方案已保存。')
            except OSError as error:
                messagebox.showerror('保存失败', str(error), parent=self)

    def open_design(self):
        filename = filedialog.askopenfilename(parent=self, title='打开连线方案', filetypes=[('连线方案', '*.json')])
        if filename:
            try:
                self.design.import_document(json.loads(Path(filename).read_text(encoding='utf-8')))
                self.notice.set('方案已载入，点击“应用连线”写入设备。')
                self.refresh()
            except (ValueError, OSError) as error:
                messagebox.showerror('方案读取失败', str(error), parent=self)

    def _hit(self, event):
        x, y = self.canvas.canvasx(event.x), self.canvas.canvasy(event.y)
        for item in reversed(self.canvas.find_overlapping(x, y, x, y)):
            for tag in self.canvas.gettags(item):
                if tag.startswith(('node:', 'gpio:')):
                    kind, number = tag.split(':')
                    return kind, int(number)
        return None

    def _canvas_click(self, event):
        hit = self._hit(event)
        if hit:
            kind, number = hit
            if kind == 'node':
                self.select_node(number)
            else:
                self.selected_gpio = number
                self.gpio_name.set(f'GPIO{number} · 电平 {self.levels.get(GPIO_LIST.index(number), "—")}')
                self.notice.set(f'GPIO{number} 是直出引脚，不经过矩阵；在右侧设置输入或输出。')
                self.refresh()

    def _hover(self, event):
        hit = self._hit(event)
        self.canvas.configure(cursor='hand2' if hit else '')
        if hit:
            kind, number = hit
            self.tooltip.set(node_label(number) + ' · 点击加入/移出当前网络' if kind == 'node'
                             else f'GPIO{number} · 直出引脚 · 点击在右侧配置')

    def draw_board(self):
        c = self.canvas
        width, height = c.winfo_width(), c.winfo_height()
        if width < 100 or height < 100:
            return
        height = max(height, 537)
        c.configure(scrollregion=(0, 0, width, height))
        c.delete('all')
        self.pin_positions = {}
        # A vector board view keeps all 40 physical holes legible at different sizes.
        for x in range(20, width, 28):
            for y in range(20, height, 28):
                c.create_oval(x, y, x + 1, y + 1, fill='#1b4166', outline='')
        c.create_text(24, 24, text='JBOX-S3', fill=INK, font=('Segoe UI', 16, 'bold'), anchor='w')
        c.create_text(24, 49, text='N16R8  /  8 SHARED NETS', fill='#7da9ce', font=('Segoe UI', 9), anchor='w')
        c.create_text(width - 24, 30, text='与实物同方向', fill='#93b6d3', anchor='e', font=('Microsoft YaHei UI', 10))
        small_x = width - 285
        c.create_text(small_x, 76, text='直出 GPIO · 不经过矩阵', fill='#a6c3db', anchor='w', font=('Microsoft YaHei UI', 9))
        c.create_rectangle(small_x - 12, 94, width - 22, 184, fill='#142335', outline='#365471')
        for row, pins in enumerate(DIRECT_HEADER):
            for col, gpio in enumerate(pins):
                x, y = small_x + 23 + col * 49, 113 + row * 49
                if gpio is None:
                    self._pin(x, y, 51, 'GND', size=13)
                else:
                    tag = f'gpio:{gpio}'
                    level = self.levels.get(GPIO_LIST.index(gpio))
                    color = '#57d6af' if level == 1 else '#8a9caf'
                    if gpio == self.selected_gpio:
                        color = ACCENT
                    c.create_rectangle(x - 15, y - 15, x + 15, y + 15, fill='#0a1725', outline=color, width=2, tags=(tag,))
                    c.create_text(x, y, text=str(gpio), fill=INK, tags=(tag,), font=('Segoe UI', 10, 'bold'))
        c.create_text(26, 115, text='点亮一组网络，连接一组端子。', anchor='w', fill=INK, font=('Microsoft YaHei UI', 12))
        c.create_text(26, 146, text='上下两孔直通；同色端子互相连接。', anchor='w', fill='#93b6d3', font=('Microsoft YaHei UI', 10))

        extra_space = height - 537
        first_y = 230 + extra_space * 0.25
        second_y = 350 + extra_space * 0.60
        start_x, end_x = 38, width - 38
        spacing = (end_x - start_x) / 19
        radius = min(15, spacing * 0.36)
        for connector, top_y in (('CN1', first_y), ('CN2', second_y)):
            c.create_text(25, top_y - 38, text=connector, fill=INK, anchor='w', font=('Segoe UI', 15, 'bold'))
            c.create_text(width - 25, top_y - 38, text='20 组直通端子 / 40 孔', fill='#93b6d3', anchor='e', font=('Microsoft YaHei UI', 9))
            c.create_rectangle(19, top_y - 22, width - 19, top_y + 66, fill='#121e2b', outline='#365471')
            for row, pins in enumerate(connector_rows(connector)):
                for col, (pin, node) in enumerate(pins):
                    x, y = start_x + col * spacing, top_y + row * 44
                    if row == 0:
                        group = self.design.group_of(node)
                        color = NET_COLORS[group] if group is not None else '#3c536a'
                        c.create_line(x, y + radius, x, y + 44 - radius, fill=color, width=2)
                    self._pin(x, y, node, str(pin), radius)

        extra_y = 480 + extra_space * 0.92
        c.create_text(25, extra_y - 35, text='矩阵 GPIO / 电源', fill=INK, anchor='w', font=('Microsoft YaHei UI', 11))
        extras = [(n, f'G{n - 39}') for n in range(40, 49)] + [(49, '5V'), (50, '3V3'), (51, 'GND')]
        for index, (node, label) in enumerate(extras):
            x = 42 + index * (width - 84) / (len(extras) - 1)
            self._pin(x, extra_y, node, label, size=14)
        # Bus overlays show the selected design without changing any hardware.
        for group, net in enumerate(self.design.nets):
            if group != self.design.active:
                continue
            points = [self.pin_positions[n][0] for n in net if n in self.pin_positions]
            if len(points) < 2:
                continue
            bus_y = first_y - 24
            xs = [x for x, y in points]
            c.create_line(min(xs), bus_y, max(xs), bus_y, fill=NET_COLORS[group], width=2, dash=(4, 3))
            for x, y in points:
                c.create_line(x, bus_y, x, y - radius - 2, fill=NET_COLORS[group], width=1, dash=(3, 5))
                c.create_oval(x - 3, bus_y - 3, x + 3, bus_y + 3, fill=NET_COLORS[group], outline='')

    def _pin(self, x, y, node, label, size=14):
        group = self.design.group_of(node)
        color = NET_COLORS[group] if group is not None else '#647b91'
        tag = f'node:{node}'
        self.pin_positions.setdefault(node, []).append((x, y))
        self.canvas.create_rectangle(x - size, y - size, x + size, y + size,
                                     fill=color if group is not None else '#23374b', outline=color,
                                     width=2, tags=(tag,))
        self.canvas.create_text(x, y, text=label, fill='#0a1725' if group is not None else INK,
                                font=('Segoe UI', 9, 'bold'), tags=(tag,))

    def close_app(self):
        self.closed = True
        for callback in self.tk.splitlist(self.tk.call('after', 'info')):
            self.after_cancel(callback)
        self.worker.stop()
        self.destroy()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='', help='可选设备 IP，也可在窗口里搜索')
    args = parser.parse_args()
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    MatrixApp(args.host).mainloop()


if __name__ == '__main__':
    main()
