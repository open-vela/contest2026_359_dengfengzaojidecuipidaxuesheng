"""One authored, editable vector and operating mechanism per registered type.

24-unit rounded silhouettes. Body and working parts are separate filled paths;
SVG export and the native LVGL masks use these exact same contours.
"""
import json
import math
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
CATALOG=json.loads((ROOT/'app/homeassistant/quick-card-catalog.json').read_text(encoding='utf-8'))['types']
ART={}

def dot(x,y,r):
    return f'M{x+r:g} {y:g}A{r:g} {r:g} 0 1 1 {x-r:g} {y:g}A{r:g} {r:g} 0 1 1 {x+r:g} {y:g}Z'

def ring(x,y,r,w=1.4):
    q=r-w
    return dot(x,y,r)+f'M{x+q:g} {y:g}A{q:g} {q:g} 0 1 0 {x-q:g} {y:g}A{q:g} {q:g} 0 1 0 {x+q:g} {y:g}Z'

def polygon(points):
    if sum(x*b-a*y for (x,y),(a,b) in zip(points,points[1:]+points[:1]))<0:points=list(reversed(points))
    return 'M'+'L'.join(f'{x:g} {y:g}' for x,y in points)+'Z'

def line(*points,w=1.15):
    p=''.join(dot(x,y,w/2) for x,y in points)
    for (x,y),(a,b) in zip(points,points[1:]):
        d=math.hypot(a-x,b-y)
        if not d:continue
        nx,ny=(y-b)*w/(2*d),(a-x)*w/(2*d)
        p+=polygon([(x+nx,y+ny),(a+nx,b+ny),(a-nx,b-ny),(x-nx,y-ny)])
    return p

def box(x,y,w,h,r=2.0):
    pts=[]
    for cx,cy,start in [(x+w-r,y+r,-90),(x+w-r,y+h-r,0),(x+r,y+h-r,90),(x+r,y+r,180)]:
        pts.extend((cx+r*math.cos(math.radians(start+a)),cy+r*math.sin(math.radians(start+a))) for a in range(0,91,15))
    return line(*pts,pts[0])

def steam(x,y):
    y=max(4,y)
    return line(*[(x+math.sin(i/18*math.pi*1.8)*.65,y+4-i/3) for i in range(19)],w=1.0)

def vents(x,y,count=4,length=8):
    return ''.join(line((x,y+i*2),(x+length,y+i*2),w=1) for i in range(count))

def drop(x,y):
    return f'M{x:g} {y:g}C{x-4:g} {y+4:g} {x-3:g} {y+6:g} {x:g} {y+6:g}C{x+3:g} {y+6:g} {x+4:g} {y+4:g} {x:g} {y:g}Z'

def blades(x,y,r=5):
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.svgLib.path import parse_path
    pen=SVGPathPen(None)
    for angle in (0,120,240):
        c,s=math.cos(math.radians(angle))*r/5,math.sin(math.radians(angle))*r/5
        parse_path('M0 0C1-1 4-1 4-3C4-5 1-5 0-4C-2-3-1-1 0 0Z',TransformPen(ReverseContourPen(pen),(c,s,-s,c,x,y)))
    return pen.getCommands()+dot(x,y,1.1)

def rays(x,y,r=3):
    return ''.join(line((x+r*math.cos(a),y+r*math.sin(a)),(x+(r+1.5)*math.cos(a),y+(r+1.5)*math.sin(a)),w=1.1) for a in [i*math.pi/4 for i in range(8)])

def add(kind,label,body,parts,detail,run=4,*,states=(),direction=(0,0),pivot=(12,12),period=2200,reverse=()):
    assert kind not in ART and kind in CATALOG
    body=[body] if isinstance(body,str) else body
    parts=[parts] if isinstance(parts,str) else parts
    ART[kind]=dict(label=label,paths=body+parts,moving=list(range(len(body),len(body)+len(parts))),
        detail=detail,kind=run,pivot=pivot,period=period,direction=direction,
        reverse=[len(body)+i for i in reverse],states=list(states))

ON=('on',)
HEAT=('running','heating','drying')
COOK=('running','cooking','preheating','heating')
CYCLE=('running','washing','rinsing','spinning','drying')
MOVE=('opening','closing')
FLOW=('running','filtering','purifying')

add('diffuser','香薰机',box(6,12,12,9,3)+line((8,11),(16,11)),[steam(10,6),steam(14,5)],'香薰机圆润底座固定，香气从顶部缓慢上升',3,states=ON,direction=(0,-1),period=3000)
add('air-conditioner','空调',box(3,3,18,10)+line((5,9),(19,9))+dot(18,6,.7),[line((7,15),(6,19)),line((12,15),(12,21)),line((17,15),(18,19))],'壁挂机与导风口固定，冷暖气流向下送出',3,states=('cooling','heating','drying','fan'),direction=(0,1))
add('air-fresh','新风机',box(4,4,16,16)+vents(7,7,3,10),[line((7,15),(10,16),(13,15),(17,16)),line((7,18),(11,19),(17,18))],'壁挂新风机面板固定，换气流从下方输出',3,states=ON,direction=(1,0))
add('air-fryer','空气炸锅',box(6,3,12,19,3)+box(8,6,8,3,1)+line((7,12),(17,12))+line((10,14),(14,14)),[steam(10,17),steam(14,17)],'炸锅抽屉与把手固定，内胆热气上升',3,states=COOK,direction=(0,-1))
add('air-monitor','空气质量监测仪',box(3,4,18,17,2)+box(6,7,12,7,1)+vents(7,17,2,10),[line((8,11),(10,9),(12,12),(15,9),(16,10))],'监测仪屏幕外框固定，检测曲线轻柔显现',4)
add('air-purifier','空气净化器',box(6,3,12,19,3)+vents(8,12,4,8)+ring(12,7,1.6),[line((3,7),(2,5),(4,3)),line((21,7),(22,5),(20,3))],'净化器立式机身与滤网固定，出风口气流向上流动',3,states=ON,direction=(0,-1))
add('airer','晾衣架',line((3,3),(21,3))+line((7,3),(7,7))+line((17,3),(17,7)),[line((4,9),(20,9))+line((7,9),(7,12),(4,14),(7,14),(7,20),(12,20),(12,14),(15,14),(12,12),(12,9))+line((16,9),(16,20),(20,20),(20,12))],'天花吊杆固定，横梁与衣物随升降机构移动',5,states=MOVE,direction=(0,1),period=3200)
add('bath-heater','浴霸',box(3,3,18,18)+line((10,4),(10,20))+line((14,4),(14,20)),[ring(6.5,8,1.8)+ring(17.5,8,1.8),ring(6.5,16,1.8)+ring(17.5,16,1.8)],'浴霸面板固定，四个取暖灯交错亮起',8,states=HEAT)
add('camera','摄像机',box(5,3,14,12,4)+line((12,15),(12,20))+line((7,22),(17,22)),[ring(12,9,3.5)+dot(12,9,1)],'摄像机外壳与支架固定，镜头局部对焦',7,states=('recording','streaming'),pivot=(12,9))
add('ceiling-fan','吊扇灯',line((12,2),(12,6))+ring(12,10,6.5)+box(10,17,4,3,1),[blades(12,10)],'吊杆与灯座固定，三片扇叶绕中心旋转',2,states=ON,pivot=(12,10),period=1700)
add('coffee-machine','咖啡机',box(5,3,14,18)+box(7,5,10,4,1)+line((8,11),(16,11))+line((6,20),(18,20))+box(9,15,6,4,1)+line((15,16),(17,16),(17,18),(15,18)),[line((11,12),(11,14),w=1)+line((13,12),(13,14),w=1)],'咖啡机与杯子固定，萃取液从出液口落入杯中',3,states=('running','brewing','grinding','heating'),direction=(0,1),period=1800)
add('cooker','电饭煲',box(4,8,16,13,4)+line((6,7),(18,7))+box(9,14,6,3,1)+line((9,5),(15,5)),[steam(9,2),steam(15,2)],'电饭煲锅体与显示屏固定，排气口蒸汽上升',3,states=COOK,direction=(0,-1))
add('curtain','窗帘',line((2,3),(22,3))+line((3,4),(3,21),(21,21),(21,4)),[line((6,6),(6,19))+line((9,5),(9,19)),line((15,5),(15,19))+line((18,6),(18,19))],'窗帘轨道与外框固定，两侧帘布相向开合',5,states=MOVE,direction=(1,0),reverse=(1,),period=3400)
add('dishwasher','洗碗机',box(5,2,14,20)+line((6,7),(18,7))+dot(8,5,.7)+line((12,5),(16,5))+box(7,10,10,10,1)+line((8,18),(16,18))+line((9,14),(9,18))+line((12,13),(12,18))+line((15,14),(15,18)),[line((9,10),(9,12),w=.9),line((12,9),(12,11),w=.9),line((15,10),(15,12),w=.9)],'洗碗机面板与碗架固定，水流从喷淋口落向餐具',3,states=CYCLE,direction=(0,1),period=2400)
add('fan','落地风扇',ring(12,9,7)+line((12,16),(12,21))+line((7,22),(17,22)),[blades(12,9)],'网罩、立柱与底座固定，扇叶持续旋转',2,states=ON,pivot=(12,9),period=1600)
add('flowerpot','花盆',line((4,13),(20,13),(18,21),(6,21),(4,13))+line((3,11),(21,11)),[line((12,11),(12,5))+'M12 8C7 9 5 6 5 3C9 3 12 4 12 8Z M12 6C13 2 17 1 20 2C20 6 16 8 12 6Z'],'花盆固定，植株叶片轻摆提示植物数据变化',6,pivot=(12,11),period=4200)
add('fridge','冰箱',box(5,2,14,20)+line((6,9),(18,9))+line((8,5),(8,7))+line((8,12),(8,16)),[line((13,13),(13,18))+line((10.8,14.2),(15.2,16.8))+line((10.8,16.8),(15.2,14.2))],'冰箱双门与把手固定，冷藏室内制冷标志呼吸',4,states=('cooling','defrosting','running'))
add('fruit-vegetable-purifier','果蔬净化机',box(3,12,18,9,3)+line((6,10),(18,10))+line((12,9),(12,5)),[ring(7,8,1.6)+ring(17,7,1.3)+dot(12,16,1)+dot(8,17,.7)+dot(16,17,.7)],'果蔬清洗槽固定，净化气泡在槽内升起',3,states=('running','washing','purifying'),direction=(0,-1))
add('garbage-can','智能垃圾桶',box(6,8,12,14,2)+line((10,12),(10,18))+line((14,12),(14,18)),[line((4,6),(20,6))+line((9,4),(15,4))],'垃圾桶桶身固定，桶盖围绕后轴开合',6,states=MOVE,pivot=(5,6),period=2600)
add('gas-concentration','燃气浓度监测仪',box(5,3,14,18)+box(7,6,10,7,1)+vents(8,16,2,8),[line((9,10),(11,8),(13,11),(15,9),w=1)],'燃气浓度仪外壳固定，屏幕中的浓度曲线反馈读数',4)
add('gas-sensor','燃气报警器',box(4,4,16,16,4)+vents(8,16,2,8),[polygon([(12,7),(9,11),(9,13),(12,15),(15,13),(15,11),(13,12)])],'报警器外壳与进气口固定，检测到燃气后火焰标志闪动',4,states=('on','detected','alarm'),period=1600)
add('gateway','网关',box(3,13,18,8,2)+line((6,13),(4,4))+line((18,13),(20,4)),[dot(7,17,1),dot(12,17,1),dot(17,17,1)],'网关与天线固定，配对或数据传输时指示灯顺序闪动',8,states=('pairing','transmitting','updating'),period=1800)
add('health-pot','养生壶',box(6,8,11,13,3)+line((7,6),(16,6))+line((9,4),(14,4))+line((17,10),(21,10),(21,16),(17,17)),[steam(10,12)+steam(14,12)],'养生壶壶体与把手固定，壶内热流轻柔上升',3,states=COOK,direction=(0,-1))
add('heater','取暖器',box(3,6,18,14,2)+line((6,20),(6,22))+line((18,20),(18,22))+vents(6,9,4,12),[steam(8,2),steam(16,2)],'取暖器格栅与支脚固定，顶部热流向上散发',3,states=HEAT,direction=(0,-1))
add('hood','油烟机',box(9,2,6,6,1)+line((9,8),(3,13),(21,13),(15,8))+line((5,16),(19,16)),[steam(8,18),steam(16,18)],'烟机罩体与烟道固定，烟气向吸入口上升',3,states=ON,direction=(0,-1),period=1600)
add('humidifier','加湿器',box(5,8,14,14,3)+ring(12,17,1.5)+line((9,6),(15,6)),[steam(9,3),steam(15,3)],'加湿器水箱固定，双股水雾从顶部上升',3,states=ON,direction=(0,-1),period=2800)
add('illumination-sensor','光照传感器',box(4,4,16,16,4)+ring(12,12,2.5),[rays(12,12,4)],'光照传感器外壳与感光中心固定，感光射线局部轮转',2,pivot=(12,12),period=7000)
add('induction-cooker','电磁炉',box(3,5,18,16,2)+ring(12,11,4.5)+line((6,18),(9,18))+dot(17,18,.8),[ring(12,11,2.3)+dot(12,11,.7)],'电磁炉面板固定，加热区线圈按加热节律呼吸',4,states=COOK)
add('integrated-stove','集成灶',box(3,10,18,12,1)+box(5,2,14,5,1)+line((6,7),(6,10))+line((18,7),(18,10))+line((12,16),(12,21)),[ring(8,13,2),ring(16,13,2)],'集成灶烟罩与双门固定，两侧炉头交替提示工作',8,states=('on','running','cooking','heating'))
add('juicer','榨汁机',box(7,14,10,8,2)+line((8,14),(6,3),(17,3),(16,14))+line((17,5),(21,5),(21,11),(17,12))+dot(12,18,1),[line((9,9),(15,9))+line((12,6),(12,12))],'榨汁机杯体与电机底座固定，杯内刀片旋转',2,states=('running','cooking','juicing','blending'),pivot=(12,9),period=1400)
add('kettle','电水壶',box(6,8,11,13,4)+line((8,6),(15,6))+line((16,8),(21,8),(21,16),(17,18))+line((6,10),(3,6),(3,11),(6,14))+line((5,22),(19,22)),[steam(10,2),steam(14,2)],'电水壶壶体、壶嘴与把手固定，沸腾蒸汽上升',3,states=('running','heating','boiling'),direction=(0,-1))
add('light','书桌台灯',line((14,21),(20,21))+line((17,21),(17,7),(10,7))+line((5,4),(10,4),(12,10),(3,10),(5,4)),[line((6,6),(9,6),w=1)],'灯架固定，暖光向地面洒下并轻柔呼吸',1,states=ON,period=2600)
add('magnet-sensor','门窗传感器',box(5,3,8,18,2)+box(16,6,3,12,1),[line((8,8),(10,8),w=1.2)],'磁吸双体固定，查看门窗状态时感应标志轻亮',4)
add('massage','按摩椅',line((4,3),(6,14),(11,18),(19,18),(21,22))+line((4,13),(3,19),(7,21))+line((17,8),(17,14),(21,14)),[ring(9,10,1.5),ring(11,15,1.5)],'按摩椅靠背与扶手固定，按摩滚轮交替反馈',8,states=('on','running','massaging'),period=1600)
add('mirror','智能镜',box(6,2,12,17,3)+line((12,19),(12,22))+line((7,22),(17,22)),[line((9,6),(9,15),w=1),line((15,6),(15,15),w=1)],'镜框与底座固定，两侧补光灯亮起',4,states=ON)
add('mosquito-dispeller','驱蚊器',box(7,5,10,16,3)+line((8,10),(16,10))+line((10,7),(14,7))+line((5,22),(19,22)),[ring(12,15,2.6,1)+line((10,15),(14,15),w=.8)],'驱蚊器外壳固定，芯片加热工作区缓慢呼吸',4,states=ON,period=3000)
add('motion-sensor','人体传感器',box(6,3,12,18,5)+ring(12,14,3),[line((8,7),(10,6),(14,6),(16,7)),line((9,10),(11,9),(13,9),(15,10))],'人体传感器机身固定，检测到活动时感应波依次亮起',8,states=('on','detected'),period=1800)
add('moxibustion','艾灸盒',box(3,14,18,8,3)+ring(12,18,2)+line((7,12),(17,12)),[steam(8,7),steam(12,5),steam(16,7)],'艾灸盒与调节旋钮固定，灸热气流向上升起',3,states=('on','running','heating'),direction=(0,-1),period=3000)
add('outlet','插座',box(4,4,16,16,3),[line((9,8),(9,11),w=1.8)+line((15,8),(15,11),w=1.8)+line((12,15),(12,17),w=1.8)],'插座面板固定，通电插孔轻柔亮起',4,states=ON)
add('oven','烤箱',box(3,4,18,18,2)+line((4,9),(20,9))+ring(7,6.5,1)+ring(17,6.5,1)+box(6,12,12,7,1),[line((9,14),(11,13),(13,15),(15,14),w=1.1)],'烤箱面板、旋钮与门框固定，炉内热流变化',4,states=COOK)
add('pet-drinking-fountain','宠物饮水机',box(3,15,18,7,3)+box(9,7,6,8,2)+line((12,7),(12,3),(17,3),(19,5)),[line((19,7),(19,12),w=1.2)+ring(18,16,1.4)],'饮水盆与立柱固定，出水口流水落回盆内',3,states=('on','running','pumping','circulating'),direction=(0,1),period=1800)
add('plant-monitor','植物监测仪',box(4,4,5,10,2)+line((6.5,14),(6.5,22))+line((15,21),(15,11)),['M15 15C10 15 10 11 10 8C15 8 17 10 15 15Z M15 12C15 7 18 5 21 5C22 9 19 13 15 12Z'],'土壤探针固定，旁侧叶片提示植物监测状态',6,pivot=(15,17),period=4200)
add('pressure-cooker','压力锅',box(5,8,14,13,4)+line((4,7),(20,7))+line((3,11),(1.5,11),(1.5,15),(5,15))+line((19,11),(22,11),(22,15),(19,15))+box(9,13,6,3,1)+line((11,4),(13,4)),[steam(12,1)],'压力锅锅体与把手固定，限压阀排出蒸汽',3,states=COOK,direction=(0,-1),period=2500)
add('printer','打印机',box(3,8,18,10,2)+line((6,8),(6,3),(18,3),(18,8))+dot(18,11,.8),[box(6,15,12,7,1)+line((9,18),(15,18),w=1)+line((9,20),(13,20),w=1)],'打印机机身与进纸口固定，输出纸张向下输送',3,states=('printing','running'),direction=(0,1),period=2200)
add('relay','继电器',box(5,3,14,18,1)+line((8,3),(8,6))+line((12,3),(12,6))+line((16,3),(16,6))+line((8,18),(8,21))+line((16,18),(16,21)),[line((8,14),(11,11),(16,11))+dot(8,14,1)+dot(16,14,1)],'继电器端子与外壳固定，内部触点亮起提示吸合',4,states=ON)
add('smoke-sensor','烟雾报警器',ring(12,12,9)+ring(12,12,6.5)+line((5,10),(7,10))+line((17,10),(19,10))+line((5,14),(7,14))+line((17,14),(19,14)),[dot(12,12,2)],'圆形烟感与环形进气口固定，报警指示灯闪动',4,states=('on','detected','alarm'),period=1500)
add('speaker','音箱',box(6,2,12,20,2)+ring(12,6,1.5)+ring(12,15,4.5),[dot(12,15,2.5)],'音箱箱体与高音单元固定，低音振膜随播放节律变化',7,states=('playing',),pivot=(12,15),period=1600)
add('submersion-sensor','水浸传感器',box(5,13,14,8,4)+line((8,21),(8,22))+line((16,21),(16,22)),[drop(12,3)],'地面水浸探头固定，检测到漏水后水滴提示闪动',4,states=('on','detected','alarm'),period=1700)
add('switch','墙壁开关',box(3,3,18,18,2)+box(7,6,10,12,1),[line((10,9),(14,9))+dot(12,15,.9)],'墙壁面板与摇杆固定，开启指示灯亮起',4,states=ON)
add('temperature-humidity-sensor','温湿度传感器',box(5,3,14,18,3)+ring(9,15,2)+line((9,6),(9,13)),[line((13,8),(16,8))+line((13,12),(16,12))+dot(14.5,16,1)],'温湿度计外壳与温度管固定，数字读数区域轻柔反馈',4)
add('towel-rack','电热毛巾架',line((5,2),(5,22))+line((19,2),(19,22))+line((5,6),(19,6))+line((5,11),(19,11))+line((5,16),(19,16))+line((5,21),(19,21))+box(8,7,8,11,1),[steam(10,3),steam(15,3)],'毛巾架与挂巾固定，加热时热流上升',3,states=HEAT,direction=(0,-1),period=2800)
add('treadmill','跑步机',line((3,19),(20,19),(22,22),(2,22),(3,19))+line((18,19),(16,5))+box(12,2,8,4,1)+line((16,8),(10,10)),[line((6,20.5),(9,20.5),w=1)+line((13,20.5),(16,20.5),w=1)],'跑步机扶手与控制台固定，跑带沿脚下向后运转',3,states=('running','walking','training'),direction=(-1,0),period=1500)
add('vacuum','扫地机器人',ring(12,12,9)+ring(12,9,2)+line((8,17),(16,17)),[line((4,18),(2,21))+line((4,18),(1,17))+line((4,18),(6,21))],'扫地机外壳与雷达固定，侧边清扫刷旋转',2,states=('cleaning','returning'),pivot=(4,18),period=1400)
add('video-doorbell','可视门铃',box(8,2,8,20,4)+ring(12,7,2),[ring(12,16,2.7)],'门铃面板与摄像头固定，呼叫按钮随门铃事件呼吸',4,states=('ringing','recording','streaming'),period=1600)
add('walking-pad','走步机',line((4,12),(18,12),(22,17),(20,21),(3,21),(2,17),(4,12))+line((4,14),(18,14))+dot(18,18,.8),[line((6,17),(10,17),w=1.2)+line((12,17),(15,17),w=1.2)],'走步机低矮机身固定，平整跑带向后流动',3,states=('running','walking','training'),direction=(-1,0),period=1900)
add('washer','洗衣机',box(5,2,14,20,2)+line((6,7),(18,7))+dot(8,4.5,.7)+line((12,4.5),(16,4.5))+ring(12,14,5.2),['M8.5 12C10 10 11 11 12 12C13 13 14 12 15 12L15.5 15C14 17 12 17 11 16C10 15 9 16 8.5 15Z'],'洗衣机机身与上方按键固定，中间滚筒旋转',2,states=CYCLE,pivot=(12,14),period=1800)
add('water-dispenser','饮水机',box(5,2,14,20,2)+line((6,7),(18,7))+dot(9,10,.9)+dot(15,10,.9)+box(9,14,6,5,1),[line((12,11),(12,13),w=1.1)],'饮水机外壳、冷热按键与杯托固定，出水流入杯中',3,states=('running','dispensing','heating','cooling'),direction=(0,1),period=1800)
add('water-heater','热水器',box(6,2,12,17,4)+ring(12,8,2.2)+line((8,19),(8,22))+line((16,19),(16,22)),[line((9,13),(11,12),(13,14),(15,13),w=1.2)],'壁挂热水器与管路固定，水箱内热流反馈加热状态',4,states=HEAT,period=2500)
add('water-purifier','净水器',box(2,8,6,14,2)+box(9,8,6,14,2)+box(16,8,6,14,2)+line((5,8),(5,4),(19,4),(19,8)),[line((5,11),(5,17),w=1.2),line((12,11),(12,17),w=1.2),line((19,11),(19,17),w=1.2)],'三支滤筒与连接管路固定，净水逐级流过滤芯',3,states=FLOW+('producing',),direction=(0,1),period=2500)

assert set(ART)==set(CATALOG), (set(CATALOG)-set(ART),set(ART)-set(CATALOG))

# Rounded, softly filled bodies continue the earlier Material/Fluent art
# direction. These fixed planes are distinct from the mechanical blue/amber
# accents; they never move with a drum, fan, paper sheet or stream of water.
def plane(x,y,w,h,r=2):
    r=min(r,w/2,h/2)
    return f'M{x+r:g} {y:g}H{x+w-r:g}Q{x+w:g} {y:g} {x+w:g} {y+r:g}V{y+h-r:g}Q{x+w:g} {y+h:g} {x+w-r:g} {y+h:g}H{x+r:g}Q{x:g} {y+h:g} {x:g} {y+h-r:g}V{y+r:g}Q{x:g} {y:g} {x+r:g} {y:g}Z'

# Base / recessed surface / light-catching face. Each plan follows the actual
# appliance structure rather than placing every device inside a generic tile.
SURFACES={
 'diffuser':[(plane(6,12,12,9,3),256),(plane(7,17,10,3,1),512),(plane(8,13,7,1,1),1024)],
 'air-conditioner':[(plane(3,3,18,10),256),(plane(4,9,16,3,1),512),(plane(5,4.5,13,1,1),1024)],
 'air-fresh':[(plane(4,4,16,16),256),(plane(5,13,14,6,1),512),(plane(6,5,12,1,1),1024)],
 'air-fryer':[(plane(6,3,12,19,3),256),(plane(7,12,10,9),512),(plane(8,6,8,3,1),1024)],
 'air-monitor':[(plane(3,4,18,17),256),(plane(6,7,12,7,1),1024),(plane(5,18,14,2,1),512)],
 'air-purifier':[(plane(6,3,12,19,3),256),(plane(7,12,10,9),512),(plane(8,4,7,5),1024)],
 'airer':[(polygon([(7,9),(7,12),(4,14),(7,14),(7,20),(12,20),(12,14),(15,14),(12,12),(12,9)]),256),(plane(16,10,4,10,1),512)],
 'bath-heater':[(plane(3,3,18,18),256),(plane(10,4,4,16,1),512),(dot(6.5,8,2.5)+dot(17.5,8,2.5)+dot(6.5,16,2.5)+dot(17.5,16,2.5),2048)],
 'camera':[(plane(5,3,14,12,4),256),(dot(12,9,4.3),512),(dot(11,8,1.6),1024)],
 'ceiling-fan':[(dot(12,10,6.5),256),(plane(10,17,4,3,1),2048),(dot(12,10,1.8),1024)],
 'coffee-machine':[(plane(5,3,14,18),256),(plane(7,11,10,9),512),(plane(7,5,10,4,1)+plane(9,15,6,4,1),1024)],
 'cooker':[(plane(4,8,16,13,4),256),(plane(5,17,14,3),512),(plane(6,7,12,3)+plane(9,14,6,3,1),1024)],
 'curtain':[(plane(4,5,6.5,16,1),256),(plane(13.5,5,6.5,16,1),512),(plane(5,6,1,13,.5),1024),(plane(15,6,1,13,.5),1024)],
 'dishwasher':[(plane(5,2,14,20),256),(plane(6,8,12,13,1),512),(plane(7,10,10,10,1),1024)],
 'fan':[(dot(12,9,7),256),(dot(12,9,5.4),1024),(plane(7,21,10,1,.5),512)],
 'flowerpot':[(polygon([(4,13),(20,13),(18,21),(6,21)]),2048),(plane(3,11,18,2,1),256)],
 'fridge':[(plane(5,2,14,20),256),(plane(6,10,12,11,1),512),(plane(7,3.5,9,1,1),1024)],
 'fruit-vegetable-purifier':[(plane(3,12,18,9,3),256),(plane(5,16,14,4,1),512),(plane(6,13,11,1,1),1024)],
 'garbage-can':[(plane(6,8,12,14),256),(plane(7,18,10,3,1),512),(plane(4,5,16,2,1),256)],
 'gas-concentration':[(plane(5,3,14,18),256),(plane(7,6,10,7,1),1024),(plane(7,15,10,5,1),512)],
 'gas-sensor':[(plane(4,4,16,16,4),256),(plane(7,15,10,4,1),512),(plane(7,5,8,1,1),1024)],
 'gateway':[(plane(3,13,18,8),256),(plane(4,18,16,2,1),512),(plane(5,14,13,1,1),1024)],
 'health-pot':[(plane(6,8,11,13,3),256),(plane(7,13,9,7),2048),(plane(8,9,2,8,1),1024)],
 'heater':[(plane(3,6,18,14),256),(plane(5,8,14,10,1),2048),(plane(5,7,12,1,1),1024)],
 'hood':[(plane(9,2,6,6,1)+polygon([(9,8),(3,13),(21,13),(15,8)]),256),(plane(5,13,14,3,1),512)],
 'humidifier':[(plane(5,8,14,14,3),256),(plane(6,16,12,5),512),(plane(7,9,9,1,1),1024)],
 'illumination-sensor':[(plane(4,4,16,16,4),256),(dot(12,12,6),2048),(dot(12,12,2),1024)],
 'induction-cooker':[(plane(3,5,18,16),256),(dot(12,11,4.5),512),(plane(5,6,12,1,1),1024)],
 'integrated-stove':[(plane(3,10,18,12,1)+plane(5,2,14,5,1),256),(plane(4,17,7,4,1)+plane(13,17,7,4,1),512),(plane(6,3,11,1,1),1024)],
 'juicer':[(plane(7,14,10,8),256),(polygon([(8,14),(6,3),(17,3),(16,14)]),256),(polygon([(8,9),(16,9),(16,13),(8,13)]),2048)],
 'kettle':[(plane(6,8,11,13,4),256),(plane(7,13,9,7,3),2048),(plane(8,10,1,5,.5),1024)],
 'light':[(polygon([(5,4),(10,4),(12,10),(3,10)]),256),(polygon([(4,8),(11,8),(12,10),(3,10)]),2048)],
 'magnet-sensor':[(plane(5,3,8,18)+plane(16,6,3,12,1),256),(plane(6,5,2,12,1),1024)],
 'massage':[("M5 3Q7 2 8 5L10 12Q11 14 14 14H19Q21 14 21 17V20H10Q6 20 5 16L3 6Q2.5 4 5 3Z",256),(plane(11,16,8,3,1),512)],
 'mirror':[(plane(6,2,12,17,3),256),(plane(8,4,8,13,1),512),(polygon([(8,12),(14,4),(16,4),(8,16)]),1024)],
 'mosquito-dispeller':[(plane(7,5,10,16,3),256),(plane(8,11,8,9,2),2048),(plane(9,6,6,1,1),1024)],
 'motion-sensor':[(plane(6,3,12,18,5),256),(dot(12,14,3),512),(dot(11,13,1),1024)],
 'moxibustion':[(plane(3,14,18,8,3),2048),(plane(5,18,14,3,1),256),(plane(6,15,10,1,1),1024)],
 'outlet':[(plane(4,4,16,16,3),256),(plane(6,6,12,12),1024),(plane(6,18,12,1,.5),512)],
 'oven':[(plane(3,4,18,18),256),(plane(6,12,12,7,1),512),(plane(5,5,13,1,1),1024)],
 'pet-drinking-fountain':[(plane(3,15,18,7,3)+plane(9,7,6,8),256),(plane(5,18,14,3,1),512),(plane(5,16,10,1,1),1024)],
 'plant-monitor':[(plane(4,4,5,10),256),(plane(5,5,1,7,.5),1024)],
 'pressure-cooker':[(plane(5,8,14,13,4),256),(plane(6,17,12,3),512),(plane(7,8,10,2,1)+plane(9,13,6,3,1),1024)],
 'printer':[(plane(3,8,18,10),256),(plane(6,3,12,5,1),1024),(plane(4,12,16,4,1),512),(plane(6,15,12,7,1),1024)],
 'relay':[(plane(5,3,14,18,1),256),(plane(7,8,10,8,1),1024),(plane(6,18,12,2,1),512)],
 'smoke-sensor':[(dot(12,12,9),256),(dot(12,12,6.5),1024),(dot(12,12,3),512)],
 'speaker':[(plane(6,2,12,20),256),(dot(12,15,4.5),512),(dot(11,14,1.5),1024)],
 'submersion-sensor':[(plane(5,13,14,8,4),256),(plane(6,17,12,3,2),512),(plane(8,14,8,1,.5),1024)],
 'switch':[(plane(3,3,18,18),256),(plane(7,6,10,12,1),512),(plane(8,7,7,1,.5),1024)],
 'temperature-humidity-sensor':[(plane(5,3,14,18,3),256),(plane(7,5,10,14),1024),(plane(7,19,10,1,.5),512)],
 'towel-rack':[(plane(8,7,8,11,1),2048),(plane(9,8,1,8,.5),1024)],
 'treadmill':[(polygon([(3,19),(20,19),(22,22),(2,22)]),256),(plane(12,2,8,4,1),512)],
 'vacuum':[(dot(12,12,9),256),(dot(12,9,2.8),512),("M6 7Q10 3 16 6L15 7Q10 5 7 9Z",1024)],
 'video-doorbell':[(plane(8,2,8,20,4),256),(dot(12,7,2.7),512),(dot(11.5,6.5,.8),1024)],
 'walking-pad':[(polygon([(4,12),(18,12),(22,17),(20,21),(3,21),(2,17)]),256),(polygon([(4,15),(18,15),(20,18),(4,18)]),512)],
 'washer':[(plane(5,2,14,20),256),(dot(12,14,5.2),512),(dot(12,14,4),256),("M9 11Q12 8 15 11L14 12Q11 10 10 13Z",1024)],
 'water-dispenser':[(plane(5,2,14,20),256),(plane(7,9,10,11),512),(plane(9,14,6,5,1)+plane(7,3,9,1,.5),1024)],
 'water-heater':[(plane(6,2,12,17,4),256),(plane(7,12,10,6,3),512),(plane(8,4,1,6,.5),1024)],
 'water-purifier':[(plane(2,8,6,14)+plane(9,8,6,14)+plane(16,8,6,14),256),(plane(3,10,1,8,.5)+plane(10,10,1,8,.5)+plane(17,10,1,8,.5),1024)],
}
assert set(SURFACES)==set(ART)
for kind,spec in ART.items():
    spec['surfaces']=SURFACES[kind]
    spec['color']=0x4e85c7
for kind in ['diffuser','humidifier','air-conditioner','fruit-vegetable-purifier','pet-drinking-fountain','submersion-sensor','water-purifier']:
    ART[kind]['color']=0x56a5b4
for kind in ['cooker','pressure-cooker','health-pot','kettle','oven','air-fryer','heater','bath-heater','moxibustion','towel-rack','water-heater','induction-cooker','integrated-stove','mosquito-dispeller','light','mirror']:
    ART[kind]['color']=0xd9a666
for kind in ['flowerpot','plant-monitor']:ART[kind]['color']=0x6ba58c
for kind in ['gas-sensor','smoke-sensor']:ART[kind]['color']=0xc98279
# A moving door, shirt or sheet carries its own material with it.
ART['airer']['surface_motion']={0:False,1:False}
ART['curtain']['surface_motion']={0:False,1:True,2:False,3:True}
ART['garbage-can']['surface_motion']={2:False}
ART['printer']['surface_motion']={3:False}
# Work signals disappear in standby. Physical mechanisms remain visible in a
# neutral finish; the green leaves and clay pot retain their material colors.
SIGNAL_TYPES='diffuser air-conditioner air-fresh air-fryer air-purifier coffee-machine cooker dishwasher fridge fruit-vegetable-purifier gas-sensor health-pot heater hood humidifier kettle motion-sensor moxibustion oven pet-drinking-fountain pressure-cooker submersion-sensor towel-rack water-dispenser water-heater water-purifier'.split()
for kind,spec in ART.items():
    spec['idle_hidden']=spec['moving'] if kind in SIGNAL_TYPES else []
    spec['natural_accent']=kind in ('flowerpot','plant-monitor')
    spec['warm_work']=kind!='flowerpot'
SYMBOLS={kind:'home_'+kind.replace('-','_') for kind in CATALOG}
CODEPOINTS={SYMBOLS[kind]:0xe100+i for i,kind in enumerate(CATALOG)}
BY_SYMBOL={SYMBOLS[kind]:spec for kind,spec in ART.items()}
assert len({''.join(spec['paths']) for spec in ART.values()})==59, 'Every registered type has its own silhouette'
