"""Per-symbol motion intent. Shared by SVG previews and native LVGL masks.

Moving parts are explicit; missing symbols fail generation instead of silently
inheriting a generic animation. Auxiliary body masks follow their owner at rest.
"""

DESIGNS = {}


def design(names, label, detail, kind, parts='accent', *, direction=(0, 0),
           pivot=(12, 12), period=2200, reverse=(), ink=None, gesture=None):
    names = names.split()
    labels = label.split('|')
    assert len(names) == len(labels), names
    for name, title in zip(names, labels):
        assert name not in DESIGNS, name
        DESIGNS[name] = dict(label=title, detail=detail, kind=kind, parts=parts,
            direction=direction, pivot=pivot, period=period, reverse=set(reverse),
            ink=ink, gesture=gesture)


# kind: light, rotation, flow, breathe, travel, sway, heartbeat, cascade, blink.
design('audio', '音乐', '音符随节拍轻摆', 6, 'all', pivot=(15, 6), period=1600)
design('video device_camera', '录像|安防相机', '机身固定，取景指示灯呼吸', 4, [],
       ink='M5 8H7V10H5Z', gesture=6)
design('list bars', '列表|菜单', '条目依次亮起，保持阅读顺序', 8, 'all')
design('ok', '确认', '勾选轻落后稳定确认', 7, 'all', period=2800)
design('close', '关闭', '交叉轻收，表达结束', 4, 'all', period=2800)
design('power', '电源', '电源竖线亮起，外环固定', 4, [0])
design('settings', '设置', '齿轮转动，中心轴固定', 2, [0], period=3600, gesture=14)
design('home device_generic', '首页|家居', '房屋固定，窗内亮起', 4, [],
       ink='M8 10H10V12H8Z M14 10H16V12H14Z', gesture=6)
design('download', '下载', '箭头向下传输，托盘固定', 5, [0], direction=(0, 1), period=1800)
design('upload', '上传', '箭头向上传输，托盘固定', 5, [1], direction=(0, -1), period=1800)
design('drive', '磁盘', '外壳固定，读写指示灯闪动', 4, [1], period=1600)
design('refresh', '刷新', '箭头围绕中心循环更新', 2, 'all', period=1800)
design('mute', '静音', '静音符号轻亮，保持关闭语义', 4, [0], period=2800)
design('volume_mid', '中音量', '扬声器固定，声波向外呼吸', 8, [1], period=1600)
design('volume_max', '高音量', '扬声器固定，声波由内向外扩散', 8, [2, 0], period=1600)
design('image file_image', '图片|图片文件', '外框固定，画面轻柔显现', 4, [1])
design('tint', '水滴', '水滴外形固定，内部高光流动', 3, [1], direction=(0, 1))
design('prev', '上一项', '箭头向左提示，边界竖线固定', 5, [1], direction=(-1, 0))
design('next', '下一项', '箭头向右提示，边界竖线固定', 5, [1], direction=(1, 0))
design('play', '播放', '播放三角轻推，表达开始', 5, 'all', direction=(1, 0))
design('pause', '暂停', '双竖线同步轻亮，保持暂停语义', 4, 'all', period=2800)
design('stop', '停止', '方形轻收，保持停止语义', 4, 'all', period=2800)
design('eject', '弹出', '上方三角轻抬，底线固定', 5, [1], direction=(0, -1))
for name, title, direction in [('left','向左',(-1,0)),('right','向右',(1,0)),
    ('up','向上',(0,-1)),('down','向下',(0,1)),('arrow_ne','打开',(1,-1)),
    ('new_line','换行',(-1,0)),('gps','定位',(1,-1))]:
    design(name, title, '沿功能方向轻推后归位', 5, 'all', direction=direction)
design('plus minus', '增加|减少', '符号轻亮，提示数值变化', 4, 'all')
design('eye_open', '可见', '外轮廓固定，眼睛轻眨', 9, [0], pivot=(12,12), period=4000)
design('eye_close', '隐藏', '隐藏标记轻亮，保持隐藏语义', 4, [2], period=3000)
design('warning', '警告', '外框固定，感叹号呼吸提醒', 4, [1,2], period=1800)
design('shuffle', '随机播放', '两路箭头交替提示', 8, [2,1,0])
design('loop', '循环', '两路循环箭头交替传递', 8, [0,1])
design('directory file_folder', '文件夹|文件夹', '夹体固定，内部内容轻柔显现', 4, [],
       ink='M6 10H15V11.5H6Z M6 13.5H12V15H6Z', gesture=6)
design('call', '通话', '听筒围绕握持点轻摆', 6, 'all', pivot=(12,12), period=1500)
design('cut', '剪切', '剪刀活动刀刃轻合', 6, [1], pivot=(12,12), period=2200)
design('copy', '复制', '前页轻移，后页固定', 5, [0], direction=(1,-1))
design('save', '保存', '外壳固定，写入区域依次确认', 8, [2,1])
design('envelope', '邮件', '信封固定，未读提示呼吸', 4, [],
       ink='M10 13H14V15H10Z', gesture=6)
design('charge', '充电', '闪电随充电节律亮起', 4, 'all', period=1600)
design('paste', '粘贴', '夹板固定，内容轻柔显现', 4, [],
       ink='M7 9H17V11H7Z M7 13H15V15H7Z', gesture=6)
design('bell', '通知', '铃身轻摆，铃舌随动', 6, 'all', pivot=(12,4), period=1800)
design('keyboard', '键盘', '键盘外壳固定，按键依次亮起', 8, list(range(2,12)), period=2600)
design('file file_document', '文档|文档文件', '纸张固定，正文逐行显现', 8, [1,0])
design('wifi', '无线网络', '信号由中心向外扩散', 8, [0,1,2], period=1800)
design('usb', 'USB', '连接端点亮起，线缆固定', 4, [],
       ink='M10.8 17.6H13.2V19.4H10.8Z', gesture=6)
design('bluetooth', '蓝牙', '蓝牙符号呼吸表达连接活动', 4, 'all', period=2000)
design('trash', '删除', '桶身固定，内容向下消隐', 3, [1,2], direction=(0,1), period=2400)
design('edit', '编辑', '笔尖沿书写方向轻移', 5, 'all', direction=(1,-1), period=1800)
design('backspace', '退格', '边框固定，删除标记向左提示', 5, [1], direction=(-1,0))
design('sd_card', '存储卡', '卡体固定，触点依次表示读写', 8, [0,1,2], period=1800)
design('bullet', '状态点', '状态点随活动轻柔呼吸', 4, 'all')
design('sparkles', '灵感', '星芒错峰闪耀', 8, [2,0,1], period=2400)
design('camera', '相机', '机身固定，镜头轻收对焦', 7, [0], pivot=(12,13), period=2400)
design('grid', '应用', '应用格子按行依次亮起', 8, [0,2,1,3], period=2400)
design('mic', '麦克风', '支架固定，拾音头随声音呼吸', 4, [0], period=1400)
design('chat', '对话', '气泡固定，文字按阅读顺序显现', 8, [3,2,1], period=2200)
design('radio', '收音机', '外壳固定，扬声器随声音呼吸', 4, [1], period=1600)
design('fish', '餐食', '餐盘固定，食物区域轻亮', 4, [3])
design('qr', '二维码', '定位方块固定，数据区顺序扫描', 8, [4,9,5,10,6,8,7,3], period=2400)
design('face', '表情', '面部轮廓固定，双眼自然轻眨', 9, [1,2], pivot=(12,9), period=4000)
design('headphones', '耳机', '耳罩内的声场轻柔呼吸', 4, [],
       ink='M4 13H6V17H4Z M18 13H20V17H18Z', gesture=6)
design('ellipsis', '更多', '三个点依次提示处理中', 8, [0,1,2], period=1600)
design('sun', '晴天', '太阳中心固定，光芒缓慢轮转', 2, list(range(8)), period=12000)
design('heart', '收藏', '心形以双拍节奏轻跳', 7, 'all', period=1800)
design('mouse', '鼠标', '鼠标固定，滚轮上下反馈', 5, [], direction=(0,1),
       ink='M11 4H13V7H11Z', gesture=9)
design('sliders', '调节', '控制点轻移，轨道固定', 5, [0,2,4], direction=(1,0), reverse=(2,))
design('cloud_sun', '多云', '云层固定，露出的阳光逐束闪耀', 8, [0,1,2,3,4], period=3800)
design('cloud', '云', '云朵轻柔水平漂移', 5, 'all', direction=(1,0), period=4800)
design('cloud_rain', '雨天', '云层固定，雨滴错峰落下', 3, [2,0,1], direction=(0,1), period=1800)
design('cloud_snow', '雪天', '云层固定，雪花轻柔飘落', 3, [0,2,3,1,4], direction=(0,1), period=3200)
design('search', '搜索', '镜框固定，镜内扫描点来回移动', 5, [], direction=(1,0),
       ink='M8 7H10V9H8Z', gesture=9)
design('toggle', '开关', '轨道固定，拨钮短距离提示', 5, [1], direction=(-1,0))
design('fan device_fan', '风扇|风扇设备', '叶片围绕中心持续旋转', 2, 'all', period=1600)
design('thermometer', '温度', '外壳固定，温度柱轻柔变化', 4, [],
       ink='M11 7H13V17H11Z', gesture=6)
design('device_lamp', '台灯', '暖光向下洒落，地面光斑轻柔呼吸', 1, [], period=2400, gesture=6)
design('device_plug', '插座', '面板固定，通电触点呼吸', 4, [0,2], period=2400)
design('device_sensor', '传感器', '中心固定，感应波由内向外扩散', 8, [1,3,0,4], period=2000)
design('device_contact', '门窗传感器', '门框固定，感应点呼吸', 4, [1])
design('device_climate', '空调', '机身固定，送风向下流动', 3, [1,3,4], direction=(0,1), period=2200)
design('device_media', '电视', '屏幕外壳固定，播放指示随节律变化', 4, [],
       ink='M10 7L15 10 10 13Z', gesture=6)
design('device_purifier', '空气净化器', '机身固定，气流从出风口流出', 3, [2,3], direction=(1,0), period=2200)
design('bulb', '灯泡', '灯座固定，灯罩亮起暖光', 4, [2], period=2400)
design('device_curtain', '窗帘', '外框固定，帘内提示开合方向', 5, [], direction=(1,0),
       ink='M8 9L6 12 8 15V13H10V11H8Z M16 9V11H14V13H16V15L18 12Z', gesture=9)
design('device_vacuum', '扫地机', '机身固定，清扫部件摆动', 6, [0], pivot=(4,19), period=1600)
design('device_humidifier', '加湿器', '外壳固定，水滴轻柔上浮', 3, [0], direction=(0,-1), period=2400)
design('device_heater', '暖气', '热流错峰向上流动', 3, [2,0,1], direction=(0,-1), period=2600)
design('device_kitchen', '厨房', '锅体固定，热气在锅内升起', 3, [], direction=(0,-1),
       ink='M8 10H9.5V12H8Z M12 10H13.5V12H12Z M16 10H17.5V12H16Z', gesture=13)
design('device_washer', '洗衣机', '机身与按键固定，中间滚筒持续旋转', 2, [1], pivot=(12,14), period=1800, gesture=4)
design('device_plant', '绿植', '花盆固定，叶片轻柔摆动', 6, [1], pivot=(12,16), period=3800)
design('device_lock', '门锁', '锁体固定，锁芯确认状态', 4, [1], period=2600)
for name, title in [('battery_empty','空电量'),('battery_1','低电量'),('battery_2','中电量'),
                    ('battery_3','高电量'),('battery_full','满电量')]:
    design(name,title,'电池边框固定，电量区域提示活动',4,[],
           ink='M9 15H15V18H9Z',gesture=6)
design('file_video', '视频文件', '胶片边框固定，播放指示亮起', 4, [],
       ink='M10 10L15 13 10 16Z', gesture=6)
design('file_audio', '音频文件', '纸张固定，音符随节拍轻摆', 6, [0], pivot=(14,12), period=1800)
design('file_text', '文本文件', '纸张固定，文本从上到下显现', 8, [3,2,1])
design('file_pdf', 'PDF 文件', '纸张固定，PDF 字母依次亮起', 8, [0,1,2])
design('file_spreadsheet', '表格文件', '表格固定，单元格提示计算', 4, [],
       ink='M11 11H14V14H11Z', gesture=6)
design('file_presentation', '演示文件', '外框固定，播放三角轻推', 5, [0], direction=(1,0))
design('file_archive', '压缩文件', '文件夹固定，拉链依次亮起', 8, [0,1,2])
design('file_package', '应用包', '包装固定，中心标记表示装载', 4, [],
       ink='M11 11H13V14H11Z', gesture=6)
design('file_code', '代码文件', '两侧括号轻微开合', 5, 'all', direction=(1,0), reverse=(0,))
design('file_font', '字体文件', '大小文字交替显示', 8, [0,1])
design('file_generic', '普通文件', '纸张固定，内容指示呼吸', 4, [],
       ink='M8 12H16V13.5H8Z M8 16H13V17.5H8Z', gesture=6)


from household_icon_design import BY_SYMBOL
for name,spec in BY_SYMBOL.items():
    design(name,spec['label'],spec['detail'],spec['kind'],spec['moving'],
           direction=spec['direction'],pivot=spec['pivot'],period=spec['period'],reverse=spec['reverse'])

def resolve(name, shapes, accents, shape_type):
    """Resolve reviewed component indexes; no whole-body fallback for frames."""
    spec = DESIGNS[name]
    parts = list(range(len(shapes))) if spec['parts'] == 'all' else (
        sorted(accents) if spec['parts'] == 'accent' else list(spec['parts']))
    assert set(parts) <= set(range(len(shapes))), (name, parts, len(shapes))
    kind = spec['kind']
    # Static shells use a local marker on hover/press as well as while working.
    extra = [(shape_type(spec['ink']), 1 | 2 | 16 | 128)] if spec['ink'] else []
    if name in ('device_lamp','home_light'):
        extra = [(shape_type('M4.6 11.2H9.7L13.4 20.2Q7.1 22.3 1.5 20.2Z'),32),
                 (shape_type('M1.5 20.5C1.5 19.6 4.2 19 7.5 19S13.5 19.6 13.5 20.5 10.8 22 7.5 22 1.5 21.4 1.5 20.5Z'),64)]
    if name in BY_SYMBOL:
        surface_motion=BY_SYMBOL[name].get('surface_motion',{})
        extra=[(shape_type(path),tone|(8192 if tone==2048 and BY_SYMBOL[name]['warm_work'] else 0)|(18|(4 if surface_motion[i] else 0) if i in surface_motion else 0))
               for i,(path,tone) in enumerate(BY_SYMBOL[name]['surfaces'])]+extra
    gesture = spec['gesture'] or {1:6,2:2,3:13,4:6,5:9,6:12,7:4,8:10,9:6}[kind]
    delays = {i:min(rank*45,225) for rank,i in enumerate(parts)} if kind in (3,8) else {}
    run_delays = {i:round(rank*700/max(len(parts),1)) for rank,i in enumerate(parts)} if kind in (3,8) else {}
    return dict(kind=gesture,pivot=spec['pivot'],direction=spec['direction'],moving=set(parts),
                reverse=spec['reverse'],delays=delays), dict(kind=kind,period=spec['period'],
                moving=set(parts),pivot=spec['pivot'],direction=spec['direction'],
                delays=run_delays,extra=extra,detail=spec['detail'],label=spec['label'],color=BY_SYMBOL.get(name,{}).get('color',0),
                idle_hidden=set(BY_SYMBOL.get(name,{}).get('idle_hidden',[])),natural_accent=BY_SYMBOL.get(name,{}).get('natural_accent',False))
