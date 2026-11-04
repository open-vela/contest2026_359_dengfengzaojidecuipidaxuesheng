'use strict';
// Data fixtures only; ui constructors/style updates call production C bindings.
globalThis.__reviewTimers=[];
globalThis.setInterval=(fn)=>{__reviewTimers.push(fn);return __reviewTimers.length;};
globalThis.__reviewDeferred=[];
globalThis.setTimeout=(fn)=>{__reviewDeferred.push(fn);return __reviewDeferred.length;};
globalThis.clearInterval=()=>{};globalThis.clearTimeout=()=>{};
globalThis.__reviewTick=()=>{__reviewTimers.slice().forEach(fn=>fn());const fn=__reviewDeferred.shift();if(fn)fn();};
globalThis.console={log(){}};
globalThis.app={getInfo:()=>__reviewAppInfo,exit(){}};
globalThis.prompt={input:__nativeInput,showToast:__nativeToast,dialog:__nativeDialog,showDialog:__nativeDialog};
globalThis.__devices=[];
globalThis.__audioKind='list';
globalThis.__clips=[{clip:'rec_000001',durationMs:12500,fileBytes:400044,valid:true},{clip:'rec_000002',durationMs:32000,fileBytes:1024044,valid:true}];
globalThis.__recording=false;
globalThis.__cameraState={busy:false,error:0,running:false};
globalThis.__cameraRecording={status:0,revision:0,video:false,path:''};
globalThis.system={
 yield:()=>({then:resolve=>__reviewDeferred.push(resolve)}),
 storage:{get:()=>null,set(){},delete(){}},
 camera:{style:()=>globalThis.__cameraStyle||0,setStyle:value=>{globalThis.__cameraStyle=value;return true;},
   aspect:()=>globalThis.__cameraAspect||0,setAspect:value=>{globalThis.__cameraAspect=value;return true;},
   takeAction:()=>{if(__cameraState.busy)return 0;const action=globalThis.__cameraAction||0;globalThis.__cameraAction=0;return action;},
   openAlbum:()=>{globalThis.__cameraAlbumRequested=true;return true;},devices:()=>__devices,status:()=>__cameraState,frames:()=>0,
   zoom:()=>globalThis.__cameraZoom||100,setZoom:percent=>{globalThis.__cameraZoom=percent;return true;},
   recordStatus:()=>__cameraRecording,setVideoMode:video=>{__cameraRecording.video=video;return true;},
   modes:()=>[{width:640,height:480,format:'MJPEG',fourcc:1196444237,intervals:[666666,1000000],defaultInterval:666666,continuous:false}],
   start(){__cameraState={busy:true,running:true,error:0};return 0;},stop(){__cameraState={busy:false,running:false,error:0};return 0;},photo:()=>({status:0,path:''}),preview:__nativePreview},
 espdl:{preview:__nativePreview,clear(){},cancel(){},status:()=>({busy:false,stage:0,preview:false,frame:0}),start:()=>1,render:()=>true},
 audio:{list(){__audioKind='list';return 1;},record(){__audioKind='record';return 2;},play(){__audioKind='play';return 3;},stop(){},remove(){__audioKind='remove';return 4;},transcript(){__audioKind='captions';return 5;},transcribe(){__audioKind='captions';return 6;}},
 hardware:{poll:()=>__audioKind==='list'?{done:true,ok:true,result:{clips:__clips}}:__audioKind==='captions'?{done:true,ok:true,result:{available:true,text:'你好今天天气怎么样',segments:[{atMs:500,text:'你好'},{atMs:1500,text:'今天'},{atMs:4000,text:'天气怎么样'}]}}:{done:false,ok:true,elapsedMs:globalThis.__audioElapsed===undefined?15000:__audioElapsed,peak:globalThis.__audioPeak===undefined?17000:__audioPeak},cancel(){}},
 pet:{getState:()=>({mode:'wander',visible:true,mood:80,hunger:20,dir:'right',bubble:'今天也要好好生活。',eventSeq:0}),setMode(){},poke(){},feed(){},say(){},chat(){},toggle(){},setSize(){},weather(){}}
};
globalThis.__haConfigured=false;
globalThis.__haPending=[];
globalThis.__haId=1;
globalThis.__haStates=[
 {entity_id:'light.desk',state:'on',attributes:{friendly_name:'书桌台灯',brightness:160,supported_color_modes:['brightness']}},
 {entity_id:'switch.power',state:'off',attributes:{friendly_name:'智能插座'}},
 {entity_id:'fan.air',state:'on',attributes:{friendly_name:'空气净化器',percentage:40,pm25:12,preset_mode:'auto',preset_modes:['auto','turbo','sleep']}},
 {entity_id:'sensor.temperature',state:'24.5',attributes:{friendly_name:'室内温度',unit_of_measurement:'C',device_class:'temperature'}},
 {entity_id:'sensor.humidity',state:'48',attributes:{friendly_name:'环境湿度',unit_of_measurement:'%',device_class:'humidity'}},
 {entity_id:'binary_sensor.door',state:'off',attributes:{friendly_name:'门磁',device_class:'door'}},
 {entity_id:'automation.evening',state:'on',attributes:{friendly_name:'傍晚灯光自动化'}},
 {entity_id:'update.system',state:'off',attributes:{friendly_name:'系统更新',installed_version:'1.0',latest_version:'1.0'}}
];
system.homeAssistantService={apiVersion:1,configure:()=>0,
 status:()=>({configured:__haConfigured,busy:false,url:'http://192.168.1.88:8123',canControl:true,canConfigure:true}),
 get(resource){const id=__haId++;const body=resource==='states'?__haStates:resource==='config'?{location_name:'审核示例家庭'}:[{domain:'light',services:{turn_on:{},turn_off:{}}},{domain:'switch',services:{turn_on:{},turn_off:{}}},{domain:'fan',services:{turn_on:{},turn_off:{}}}];__haPending.push({requestId:id,busy:false,done:true,cached:resource==='states',status:200,error:0,body:JSON.stringify(body)});return id;},
 getState(entity){const id=__haId++;__haPending.push({requestId:id,busy:false,done:true,status:200,error:0,body:JSON.stringify(__haStates.find(e=>e.entity_id===entity))});return id;},
 control:()=>-16,poll:()=>__haPending.shift()||{busy:false,done:false},close(){}
};
