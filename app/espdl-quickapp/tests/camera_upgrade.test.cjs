// SPDX-License-Identifier: Apache-2.0
const {test}=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm'),fs=require('node:fs'),path=require('node:path');
const source=fs.readFileSync(path.join(__dirname,'../overlay/apps/system/desktop/camera/app.js'),'utf8');
function fixture(width=1024,height=504,dark=false){
  const widgets=[],calls=[];let touch,back,zoom=100,style=0,aspect=0,action=0,album=0,tick,now=1000,scans=0,shutterReady=false,frame=[],state={busy:false,error:0};
  let devices=Array.from({length:9},(_,i)=>({id:'usb:'+i,generation:1,name:'USB Camera '+i,transport:'usb',previewSupported:true}));
  let modes=[{width:640,height:480,index:1,intervals:[666666,1000000],continuous:false}];
  let recording={status:0,revision:0,video:false,path:''};
  function add(text,x,y,w,h,action){widgets.push({text,x,y,w,h,action,hidden:false});return widgets.length-1;}
  const ui={getSize:()=>({width,height}),card:dark?0x22252f:0xffffff,surface:dark?0x2e303d:0xf5f5f7,
    primary:0x252d46,secondary:0x616981,accent:0x77658f,background(){},cameraReference(){return 1;},cameraReferenceZoom(){},cameraReferenceControls:(scene,style,video,ready,menu,filters,...geometry)=>{shutterReady=ready;frame=geometry;},setOpacity:(id,opacity)=>widgets[id].opacity=opacity,onTouch:fn=>touch=fn,
    onBack:fn=>back=fn,text:(t,x,y,font)=>add(t,x,y,20,font+4),panel:(x,y,w,h)=>add('',x,y,w,h),
    button:(t,x,y,w,h,action)=>add(t,x,y,w,h,action),
    setPos:(id,x,y)=>Object.assign(widgets[id],{x,y}),setSize:(id,w,h)=>Object.assign(widgets[id],{w,h}),setStyle:(id,s)=>Object.assign(widgets[id],s),
    setColor(){},setHidden:(id,hidden)=>widgets[id].hidden=hidden,setText:(id,text)=>widgets[id].text=text};
  const camera={aspect:()=>aspect,setAspect:value=>{aspect=value;return true;},style:()=>style,setStyle:value=>{style=value;return true;},takeAction:()=>{if(state.busy)return 0;const next=action;action=0;return next;},openAlbum:()=>{album++;return true;},zoom:()=>zoom,setZoom:p=>{if(![100,150,200,300].includes(p))return false;zoom=p;return true;},devices:()=>{scans++;return devices;},modes:()=>modes,status:()=>state,frames:()=>0,
    recordStatus:()=>recording,setVideoMode:video=>{recording.video=video;return true;},
    photoStatus:()=>({status:0,path:''}),stop(){calls.push('stop');state={busy:false,error:0};},
    startDevice(...args){calls.push(args);state={busy:true,error:0};return true;}};
  const context=vm.createContext({ui,system:{camera},Date:{now:()=>now},setInterval:fn=>{tick=fn;}});
  vm.runInContext(source,context);
  return {widgets,calls,app:context.CameraPreview,touch:(state,x,y)=>touch(state,x/1024,y/600),tick(){now+=500;tick();},scans:()=>scans,
    nativeAction:value=>action=value,albumRequests:()=>album,shutterReady:()=>shutterReady,frame:()=>frame,devices:value=>devices=value,modes:value=>modes=value,state:value=>state=value,
    recording:value=>recording={...recording,...value},back:()=>back(),
    click(text){const w=widgets.find(w=>w.text===text&&w.action&&!w.hidden);assert(w,text);if(w.enabled!==false)w.action();}};
}

test('system back closes camera settings or filters without starting capture',()=>{
  const f=fixture(1024,600);
  f.click('切换相机');assert(f.app.settings());
  assert.equal(f.back(),true);assert.equal(f.app.settings(),false);
  assert.equal(f.back(),false);
  f.click('色彩风格');assert(f.app.filters());
  assert.equal(f.back(),true);assert.equal(f.app.filters(),false);
  assert.equal(f.back(),false);assert.deepEqual(f.calls,[]);
});
test('camera controls fit compact and board viewports in both themes, including paged devices',()=>{
  for(const [w,h] of [[640,400],[800,450],[1024,504],[1024,600]])for(const dark of [false,true]){
    const f=fixture(w,h,dark);f.click('切换相机');
    for(let page=0;page<3;page++){
      for(const item of f.widgets.filter(v=>!v.hidden)){
        assert(item.x>=0&&item.y>=0&&item.x+item.w<=w&&item.y+item.h<=h,JSON.stringify({w,h,item}));
      }
      if(page<2)f.click('下一页');
    }
    assert(f.widgets.length+1<=64); // Include the native camera scene handle.
    assert(f.widgets.some(v=>!v.hidden&&v.text.includes('USB Camera 8')));
    f.click('上一页');assert.equal(f.calls.length,0,'paging must not open hardware');
  }
});
test('video mode preserves mic notice and prevents restart until saving completes',()=>{
  const f=fixture();f.click('切换录制');assert.equal(f.app.video(),true);
  f.app.start();f.tick();f.recording({status:3,revision:1});f.state({busy:false,error:0});f.tick();
  assert.equal(f.widgets.find(w=>w.text==='开始预览').enabled,false);
  assert(f.widgets.some(w=>w.text.includes('正在保存录像')));
  const n=f.calls.length;f.app.start();assert.equal(f.calls.length,n);
  f.recording({status:4,path:'/sdcard/videos/VID_test.avi'});f.tick();
  assert.equal(f.widgets.find(w=>w.text==='开始预览').enabled,true);
  assert(f.widgets.some(w=>w.text.includes('录像已保存')));
  f.recording({status:-28,revision:2});f.tick();assert(f.widgets.some(w=>w.text==='SD 卡空间不足'));
});
test('camera discovery is throttled while idle and suspended during preview; disconnect clears start',()=>{
  const f=fixture();let before=f.scans();
  for(let i=0;i<3;i++)f.tick();assert.equal(f.scans(),before);
  f.tick();assert.equal(f.scans(),before+1);
  f.app.start();f.tick();assert.equal(f.calls.filter(Array.isArray).length,1);
  before=f.scans();for(let i=0;i<20;i++)f.tick();assert.equal(f.scans(),before);
  f.app.stop();f.devices([]);f.app.refresh();
  assert.equal(f.widgets.find(v=>v.text==='开始预览').enabled,false);
  f.click('切换相机');assert(f.app.settings());assert(f.widgets.some(v=>!v.hidden&&v.text==='\ue002'));
});
test('camera chooses a supported device and rejects modes without valid frame intervals',()=>{
  const f=fixture();f.devices([{id:'usb:0',generation:2,name:'Unsupported',transport:'usb',previewSupported:false},
    {id:'csi:0',generation:2,name:'CSI Camera',transport:'csi',previewSupported:true}]);f.app.refresh();
  f.app.start();f.tick();assert.equal(f.calls.find(Array.isArray)[0],'csi:0');f.app.stop();
  f.devices([{id:'usb:1',generation:3,name:'Bad modes',transport:'usb',previewSupported:true}]);
  f.modes([{index:1,width:640,height:480,intervals:[0,-1,NaN],continuous:false}]);f.app.refresh();
  assert.equal(f.widgets.find(v=>v.text==='开始预览').enabled,false);
  const count=f.calls.filter(Array.isArray).length;f.app.start();f.tick();assert.equal(f.calls.filter(Array.isArray).length,count);
});

test('reference camera menu selects and dismisses without opening hardware, preserving the chosen generation',()=>{
  for(const [w,h] of [[640,400],[1024,600]])for(const dark of [false,true]){
    const f=fixture(w,h,dark);f.click('切换相机');
    assert(f.app.settings());assert.equal(f.app.page(),0);
    f.click('下一页');assert.equal(f.app.page(),1);
    f.click('USB Camera 4');assert(!f.app.settings(),'selection dismisses the picker');
    assert.equal(f.calls.length,0,'selection alone does not activate a camera');
    f.app.start();f.tick();assert.equal(f.calls.find(Array.isArray)[0],'usb:4');
    f.click('切换相机');
    assert(!f.app.settings(),'busy camera selection cannot open the picker');
    f.app.stop();
    f.click('切换相机');f.click('USB Camera 5');f.app.start();f.tick();assert.equal(f.calls.filter(Array.isArray).at(-1)[0],'usb:5');
  }
});

test('vertical zoom gestures change real digital zoom, clamp both ends, support taps and ignore other regions',()=>{
  const f=fixture(1024,600);assert.equal(f.app.zoom(),100);
  f.touch('down',732,390);f.touch('move',732,330);f.touch('up',732,330);
  assert.equal(f.app.zoom(),150);
  f.touch('down',732,390);f.touch('move',732,210);f.touch('up',732,210);
  assert.equal(f.app.zoom(),300);
  f.touch('down',732,210);f.touch('move',732,430);f.touch('up',732,430);
  assert.equal(f.app.zoom(),100);
  f.touch('down',732,270);f.touch('up',732,270);assert.equal(f.app.zoom(),200);
  f.touch('down',500,390);f.touch('move',500,210);f.touch('up',500,210);assert.equal(f.app.zoom(),200);
  f.touch('down',732,390);f.touch('cancel',732,200);assert.equal(f.app.zoom(),200);
  f.click('切换相机');f.touch('down',732,390);f.touch('move',732,210);f.touch('up',732,210);
  assert.equal(f.app.zoom(),200,'open settings owns the gesture');
  assert.equal(f.calls.length,0,'zoom never starts or stops hardware');
});

test('annotated controls select real colour styles, request album and resume a selected live camera',()=>{
  const f=fixture();
  assert(!f.widgets.some(w=>!w.hidden&&w.action&&w.x<126),'idle page has no left control targets');
  f.click('色彩风格');assert.equal(f.app.style(),0);f.click('鲜艳');assert.equal(f.app.style(),1);
  f.click('色彩风格');f.click('黑白');assert.equal(f.app.style(),2);
  f.click('色彩风格');f.click('原色');assert.equal(f.app.style(),0);
  f.click('预览相册');assert.equal(f.albumRequests(),1);assert.equal(f.calls.length,0);
  f.app.start();f.tick();f.nativeAction(3);f.state({busy:false,error:0});f.tick();
  assert(f.app.settings());
  f.click('USB Camera 2');assert(!f.app.settings());f.tick();
  assert.equal(f.calls.filter(Array.isArray).at(-1)[0],'usb:2');
});

test('device picker owns toolbar actions and cancels an unfinished zoom gesture',()=>{
  const f=fixture(1024,600);
  f.touch('down',732,390);
  f.click('切换相机');
  f.click('色彩风格');f.click('切换录制');f.click('预览相册');f.click('开始预览');
  assert.equal(f.app.style(),0);assert.equal(f.app.video(),false);
  assert.equal(f.albumRequests(),0);assert.equal(f.calls.length,0);
  f.click('完成');
  f.touch('move',732,210);f.touch('up',732,210);
  assert.equal(f.app.zoom(),100);
  f.click('切换录制');assert.equal(f.app.video(),true);
  f.click('色彩风格');f.click('鲜艳');assert.equal(f.app.style(),1);
});

test('minimal idle view keeps saving failures visible and follows discovery',()=>{
  const f=fixture(1024,600);
  assert(f.widgets.some(w=>!w.hidden&&w.text==='按快门开始取景'));
  assert(!f.widgets.some(w=>w.text==='开始取景'&&w.action),'shutter is the single start action');
  f.click('切换录制');assert(f.widgets.some(w=>!w.hidden&&w.text.includes('麦克风开启')));
  f.recording({status:-28,revision:1});f.tick();
  const status=f.widgets.find(w=>w.text==='SD 卡空间不足');
  assert(status&&!status.hidden);assert.equal(status.textColor,0xff8178);
  f.click('切换相机');assert(status.hidden);f.click('完成');assert(!status.hidden);
  f.recording({status:0,revision:2});f.devices([]);f.app.refresh();
  assert(f.widgets.some(w=>!w.hidden&&w.text==='未连接摄像头'));
  f.click('切换相机');assert(f.app.settings());
});

test('closing the device menu with its toggle resumes a live session exactly once',()=>{
  const f=fixture(1024,600);
  assert(f.widgets.some(w=>!w.hidden&&w.text==='640 x 480 · 15 fps'));
  f.app.start();f.tick();assert.equal(f.calls.filter(Array.isArray).length,1);
  f.nativeAction(3);f.state({busy:false,error:0});f.tick();
  assert(f.app.settings());
  f.click('切换相机');
  assert(!f.app.settings());
  f.tick();assert.equal(f.calls.filter(Array.isArray).length,2);
  f.tick();assert.equal(f.calls.filter(Array.isArray).length,2);
});

test('shutter availability follows the actual start action through menus, saving and disconnect',()=>{
  const f=fixture(1024,600);assert.equal(f.shutterReady(),true);
  f.click('切换相机');assert.equal(f.shutterReady(),true);
  assert.equal(f.widgets.find(w=>w.text==='菜单快门').enabled,true);
  f.click('完成');assert.equal(f.shutterReady(),true);
  f.recording({status:3,revision:1});f.tick();assert.equal(f.shutterReady(),false);
  f.recording({status:4,revision:2});f.tick();assert.equal(f.shutterReady(),true);
  f.devices([]);f.app.refresh();assert.equal(f.shutterReady(),false);
});

test('outside taps dismiss without passing through; drags, cancellation and inside taps keep the menu',()=>{
  const f=fixture(1024,600);f.click('切换相机');
  const open=()=>f.app.settings();
  for(const end of ['move','cancel','inside']){
    const x=end==='inside'?808:500,y=300;
    f.touch('down',x,y);
    if(end==='move')f.touch('move',x,y-60);
    f.touch(end==='cancel'?'cancel':'up',x,end==='move'?y-60:y);
    f.click('收起设置');assert(open(),end);
  }
  f.touch('down',500,300);f.touch('up',500,300);f.click('收起设置');
  assert(!open());f.tick();assert.equal(f.calls.length,0);assert.equal(f.app.zoom(),100);
});

test('single-camera and empty menus use only icons in the right control area',()=>{
  const f=fixture(1024,600);
  f.devices([{id:'csi:0',generation:2,name:'CSI Camera',transport:'csi',previewSupported:true}]);
  f.app.refresh();f.click('切换相机');
  assert(!f.widgets.some(w=>!w.hidden&&['上一页','下一页','1 / 1'].includes(w.text)));
  const menu=f.widgets.find(w=>!w.action&&w.x===800&&w.y===24);
  assert(menu&&menu.w===176);
  const checkIcons=()=>{
    for(const widget of f.widgets.filter(w=>!w.hidden&&w.opacity!==0)){
      if(widget.text)assert(/^[\ue000-\uf8ff]$/.test(widget.text),widget.text);
      assert(widget.x>=800,'menu leaves the viewfinder unobstructed');
    }
  };
  checkIcons();
  assert.equal(f.widgets.find(w=>w.text==='1024 x 600'&&w.action).enabled,false);
  f.devices([]);f.app.refresh();
  checkIcons();assert.equal(f.calls.filter(Array.isArray).length,0);
});

test('icon quality controls preserve resolution and interval selection without starting hardware',()=>{
  const f=fixture(1024,600);
  f.modes([{index:1,width:640,height:480,intervals:[666666,1000000],continuous:false},
    {index:2,width:1280,height:720,intervals:[333333,666666],continuous:false}]);
  f.devices([{id:'usb:7',generation:3,name:'External camera',transport:'usb',previewSupported:true}]);
  f.app.refresh();const before=f.calls.length;f.click('切换相机');f.click('640 x 480');
  assert(f.app.settings());f.click('15 fps');assert.equal(f.calls.length,before);
  f.app.start();f.tick();
  assert.deepEqual(f.calls.find(Array.isArray),['usb:7',3,2,333333]);
});

test('the large shutter remains available in settings and starts the selected camera once',()=>{
  const f=fixture(1024,600);f.click('切换相机');
  assert(f.app.settings());assert.equal(f.shutterReady(),true);
  f.click('菜单快门');f.tick();
  assert(!f.app.settings());assert.equal(f.calls.filter(Array.isArray).length,1);
  f.tick();assert.equal(f.calls.filter(Array.isArray).length,1);
});

test('filter cards open without applying, own pointer gestures, and dismiss without changing colour',()=>{
  const f=fixture(1024,600);f.click('色彩风格');
  assert(f.app.filters());assert.equal(f.app.style(),0);assert(f.shutterReady());
  f.touch('down',732,390);f.touch('move',732,330);f.touch('up',732,330);
  f.click('收起滤镜');assert(f.app.filters());assert.equal(f.app.zoom(),100);
  f.click('切换录制');f.click('预览相册');assert(!f.app.video());assert.equal(f.albumRequests(),0);
  f.touch('down',500,300);f.touch('up',500,300);f.click('收起滤镜');
  assert(!f.app.filters());assert.equal(f.app.style(),0);assert.equal(f.calls.length,0);
  f.click('色彩风格');f.click('色彩风格');assert(!f.app.filters());
});

test('filter card choice applies once and the large shutter can start preview from the picker',()=>{
  const f=fixture(1024,600);f.click('色彩风格');f.click('黑白');
  assert.equal(f.app.style(),2);assert(!f.app.filters());assert.equal(f.calls.length,0);
  f.click('色彩风格');assert.equal(f.app.style(),2);f.click('滤镜快门');f.tick();
  assert(!f.app.filters());assert.equal(f.calls.filter(Array.isArray).length,1);
});

test('six colour cards fit the widget budget and each applies its matching style',()=>{
  const f=fixture(1024,600);
  ['原色','鲜艳','黑白','暖色','冷色','复古'].forEach((name,index)=>{
    f.click('色彩风格');assert(f.app.filters());
    const card=f.widgets.find(w=>!w.hidden&&w.text===name);
    assert.equal(card.x,780);
    assert.equal(card.y,124+index*48);
    assert.equal(card.w,40);assert.equal(card.h,40);
    assert(card.x+card.w<836,'cards leave the large shutter clear');
    f.click(name);assert.equal(f.app.style(),index);assert(!f.app.filters());
    f.tick();assert.equal(f.app.style(),index);
  });
  assert(f.widgets.length+1<=64);assert.equal(f.calls.length,0);
});

test('viewfinder geometry follows the selected camera, resolution and zoom immediately',()=>{
  const f=fixture(1024,600);
  assert.deepEqual(f.frame(),[640,480,896,100,0]);
  f.touch('down',732,390);f.touch('up',732,330);
  assert.deepEqual(f.frame(),[640,480,896,150,0]);
  f.devices([{id:'csi:0',generation:1,name:'CSI',transport:'csi',previewSupported:true}]);
  f.app.refresh();assert.deepEqual(f.frame(),[1024,600,1024,150,0]);
  f.modes([{width:1280,height:720,index:1,intervals:[666666],continuous:false},
    {width:640,height:480,index:2,intervals:[666666],continuous:false}]);
  f.devices([{id:'usb:0',generation:2,name:'USB',transport:'usb',previewSupported:true}]);
  f.app.refresh();assert.deepEqual(f.frame(),[640,480,896,150,0]);
  f.click('切换相机');f.click('640 x 480');
  assert.deepEqual(f.frame(),[1280,720,896,150,0]);
  assert.equal(f.calls.filter(Array.isArray).length,0,'geometry changes never start camera hardware');
});

test('aspect control cycles four ratios, works in settings and is isolated from filter and recording menus',()=>{
  const f=fixture(1024,600);
  for(const aspect of [1,2,3,4,1]){f.click('画幅比例');assert.equal(f.frame()[4],aspect);}
  f.click('切换相机');f.click('画幅比例');assert.equal(f.frame()[4],2);assert(f.app.settings());
  f.click('完成');f.click('色彩风格');
  assert(f.widgets.find(w=>w.text==='画幅比例').hidden);
  f.click('滤镜开关');f.recording({status:2,video:true,revision:1});f.tick();
  f.click('画幅比例');assert.equal(f.frame()[4],2);
  assert(f.widgets.length+1<=64);assert.equal(f.calls.filter(Array.isArray).length,0);
});
