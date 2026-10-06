// 无外部依赖的网页协议/安全事件测试：node test/ui_contract.mjs
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const html=fs.readFileSync(new URL('../web/index.html',import.meta.url),'utf8');
const blocks=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
assert.equal(blocks.length,2);
assert(!/<script[^>]+src=|<link[^>]+href=["']https?:/i.test(html),'离线网页不能引用 CDN');
const elements=new Map(),intervals=[],windowEvents={},documentEvents={};
const fileMode=process.argv.includes('--file');
const canvasContext=new Proxy({},{get:(o,k)=>o[k]??(()=>{})});
class Element {
  constructor(tag='div'){this.tagName=tag.toUpperCase();this.children=[];this.style={};this.dataset={};this.events={};this.value='';this.checked=false;this.textContent='';this.className='';this._classes=new Set();this.classList={add:k=>this._classes.add(k),remove:k=>this._classes.delete(k),contains:k=>this._classes.has(k),toggle:(k,state)=>{if(state===undefined)state=!this._classes.has(k);if(state)this._classes.add(k);else this._classes.delete(k)}}}
  set id(value){this._id=value;elements.set(value,this)} get id(){return this._id}
  set innerHTML(value){this._html=value;for(const match of value.matchAll(/<([\w]+)[^>]*id="([^"]+)"[^>]*>/g)){const el=new Element(match[1]);el.id=match[2];this.children.push(el)}}
  get innerHTML(){return this._html||''} get childElementCount(){return this.children.length} get lastChild(){return this.children.at(-1)}
  append(...items){for(const item of items){if(item&&typeof item==='object')item.parent=this;this.children.push(item)}} prepend(item){item.parent=this;this.children.unshift(item)}
  replaceChildren(...items){this.children=[];this.append(...items)} remove(){if(this.parent)this.parent.children=this.parent.children.filter(e=>e!==this)}
  querySelector(selector){if(!this._query)this._query={};return this._query[selector]??=new Element(selector)}
  addEventListener(type,fn){this.events[type]=fn} setAttribute(name,value){this.attributes??={};this.attributes[name]=value} add(option){this.children.push(option)}
  getContext(){return canvasContext} getBoundingClientRect(){return {left:0,top:0,width:270,height:270,right:270,bottom:270}}
  setPointerCapture(){} reportValidity(){return true} click(){this.onclick?.({target:this})} closest(){return null}
}
for(const m of html.matchAll(/<([\w]+)([^>]*\bid="([^"]+)"[^>]*)>/g)){const el=new Element(m[1]);el.id=m[3];el.width=520;el.height=400;const value=m[2].match(/\bvalue="([^"]*)"/);if(value)el.value=value[1];const className=m[2].match(/\bclass="([^"]*)"/);if(className){el.className=className[1];className[1].split(' ').forEach(c=>el.classList.add(c))}}
const tabs=['workbench','precise','settings'].map(name=>{const el=new Element('button');el.dataset.tab=name;return el});
const directionButtons=[...html.matchAll(/data-dir="([^"]+)"/g)].map(match=>{const el=new Element('button');el.dataset.dir=match[1];return el});
const motion=[...elements.values()].filter(e=>e.classList.contains('motion'));
const sent=[],fetchCalls=[];class WS {static OPEN=1;constructor(url){this.url=url;this.readyState=1}send(value){sent.push(JSON.parse(value))}}
const context={console,Date,Math,Number,Array,Object,JSON,Error,TextEncoder,Blob,URL,URLSearchParams,Option:function(label,value){this.label=label;this.value=value},WebSocket:WS,
  location:{protocol:fileMode?'file:':'http:',host:'192.168.4.1',hostname:'192.168.4.1',origin:'http://192.168.4.1'},
  document:{hidden:false,body:new Element('body'),getElementById:id=>elements.get(id),createElement:tag=>new Element(tag),createTextNode:text=>({textContent:text}),querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='[data-dir]'?directionButtons:selector==='.tab'?['workbench','precise','settings'].map(id=>elements.get(id)):selector==='.motion'?motion:[],addEventListener:(type,fn)=>documentEvents[type]=fn},
  window:{addEventListener:(type,fn)=>windowEvents[type]=fn},setInterval:fn=>intervals.push(fn),setTimeout:()=>1,clearTimeout:()=>{},confirm:()=>true,
  fetch:async(path,init)=>{fetchCalls.push({path,init});return {ok:true,json:async()=>({ok:true})}}};
vm.createContext(context);blocks.forEach((b,i)=>vm.runInContext(b,context,{filename:'web-script-'+i+'.js'}));
const run=code=>vm.runInContext(code,context);
// 默认值必须与真实 Config 构造及生产 configJson 输出保持一致，不能只核对字段名。
// fixture 由 run_host_tests.ps1 生成；缺失时要求先跑主机检查，禁止跳过这项交付门控。
const defaultsFixtureUrl=new URL('../test-artifacts/config-defaults.json',import.meta.url);
assert(fs.existsSync(defaultsFixtureUrl),'请先执行 run_host_tests.ps1 生成实际固件默认配置 fixture');
const firmwareDefaults=JSON.parse(fs.readFileSync(defaultsFixtureUrl,'utf8'));
assert.equal(firmwareDefaults.revision,1);
assert.equal(firmwareDefaults.hasApPass,true);
assert.equal(firmwareDefaults.hasStaPass,false);
assert.equal(firmwareDefaults.hasCloudDeviceKey,false);
for(const key of ['revision','hasApPass','hasStaPass','hasCloudDeviceKey'])delete firmwareDefaults[key];
const uiDefaults=JSON.parse(run('JSON.stringify(UI_DEFAULT_CONFIG)'));
function assertDefaultsEqual(actual,expected,path='UI_DEFAULT_CONFIG') {
  assert.equal(typeof actual,typeof expected,path+' 类型与固件不一致');
  if(typeof expected==='number') {
    assert(Number.isFinite(actual)&&Number.isFinite(expected),path+' 必须为有限数值');
    // C++ float 与 JS double 的序列化尾数不同；仅容许单精度舍入，不容许参数差异。
    assert(Math.abs(actual-expected)<=1e-6*Math.max(1,Math.abs(expected)),path+' 默认值与固件不一致：'+actual+' / '+expected);
  } else if(Array.isArray(expected)) {
    assert(Array.isArray(actual),path+' 必须为数组');assert.equal(actual.length,expected.length,path+' 数组长度不一致');
    expected.forEach((value,index)=>assertDefaultsEqual(actual[index],value,path+'['+index+']'));
  } else if(expected!==null && typeof expected==='object') {
    assert(actual!==null && !Array.isArray(actual),path+' 必须为对象');
    assert.deepEqual(Object.keys(actual).sort(),Object.keys(expected).sort(),path+' 全字段集合与固件不一致');
    for(const key of Object.keys(expected))assertDefaultsEqual(actual[key],expected[key],path+'.'+key);
  } else assert.equal(actual,expected,path+' 默认值与固件不一致');
}
assertDefaultsEqual(uiDefaults,firmwareDefaults);
console.log('PASS: UI defaults match real configJson(Config()) for all '+Object.keys(firmwareDefaults).length+' fields including PID and wheel arrays');
if(fileMode){
  assert.equal(run('ws'),null,'file:// 离线模式禁止伪连接');
  assert.equal(sent.length,0);assert(elements.get('qrcode').children.length===1,'离线二维码仍须本地可用');
  console.log('PASS: file:// 模式初始化无 JavaScript 异常，保持离线且生成真实 QR');
  process.exit(0);
}
run('drive(.5,.8)');assert.equal(sent.length,0,'无握手/遥测不能驱动车辆');
run('ws.onmessage({data:JSON.stringify({t:"hello",client:7,session:"0123456789abcdef0123456789abcdef"})})');
assert.equal(fetchCalls[0].path,'/api/config','hello 之后、首次遥测之前必须能读取配置');
assert.equal(fetchCalls[0].init.headers['X-CombatBot-Session'],'0123456789abcdef0123456789abcdef');
const telemetry={t:'tm',configRevision:7,resultId:0,calibrationSessionId:55,owner:0,st:'idle',fault:'',estop:false,progress:0,rpm:[0,0,0,0],spd:0,yaw:0,odo:0,bat:24,acc:[0,0,1],gr:[false,true,false,false],e18:[false,false,false],ir:[50,70,90,110,130,140],ioOk:true,accelOk:true,net:'AP_ONLY'};
context.testTelemetry=telemetry;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');
run('drive(.5,.8)');assert.equal(sent.at(-1).t,'drv');assert.equal(sent.at(-1).x,.5,'死区/expo 不应在网页重复施加');assert.equal(sent.at(-1).y,.8);
intervals[0]();assert(sent.some(m=>m.t==='hb'),'运动期间必须每 200 ms 发心跳');
windowEvents.blur();assert.equal(sent.at(-1).t,'stop','失焦立即请求停车');
run('start({t:"move",dist:30})');assert.equal(sent.at(-1).t,'move');intervals[0]();assert.equal(sent.at(-1).t,'hb','精准动作也必须保持心跳');
telemetry.owner=99;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');const before=sent.length;run('drive(1,1)');assert.equal(sent.length,before,'其他客户端持有动作时禁止发驱动');
telemetry.owner=0;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');elements.get('joystick').onpointerdown({pointerId:42,clientX:135,clientY:80,preventDefault(){}});assert.equal(sent.at(-1).t,'drv');elements.get('joystick').events.pointercancel({pointerId:42});assert.equal(sent.at(-1).t,'stop','pointercancel 必须停车');
elements.get('estop').click();assert.equal(sent.at(-1).t,'estop');
run('ws.onmessage({data:JSON.stringify({t:"done",type:"odo_cal",target:100,actual:99.5,err:-.5,id:2})})');assert.equal(run('recentCal.odo'),false,'其他客户端标定结果只展示，不能授权本页应用');
run('calibration("odo");ws.onmessage({data:JSON.stringify({t:"done",type:"odo_cal",target:100,actual:99.5,err:-.5,id:3,sessionId:55,configRevision:7,calibrationPulses:400})})');assert.equal(run('recentCal.odo'),true);
run('calibration("turn");ws.onmessage({data:JSON.stringify({t:"done",type:"turn_cal",target:360,actual:359,err:-1,id:4,sessionId:55,configRevision:7,calibrationTurnFactor:1})})');assert.equal(run('recentCal.turn'),true);
telemetry.st='done';run('ws.onmessage({data:JSON.stringify(testTelemetry)});start({t:"move",dist:30})');assert.equal(sent.at(-1).t,'move','完成动作后的 done 状态允许启动下一个动作');
const configKeys=run('allFields.map(f=>f[0])');for(const key of ['apPass','staPass','speedPid','invert','digitalDebounceMs','gyroAxis','gyroSign','servoCenterUs','edgeProtection','irProtection','tiltProtection','stallTimeoutMs','telemetryHz','irScale','impactThresholdG','irSlowdownCm','irFailSafeStop','turnAcceleration','turnDeceleration'])assert(configKeys.includes(key),'缺少配置 '+key);
assert(!configKeys.includes('coastOnLoss'),'不能提供无硬件效果的刹车开关');
const configSource=fs.readFileSync(new URL('../src/config.cpp',import.meta.url),'utf8');
const serverFields=new Set([...configSource.matchAll(/\bX\((\w+)\)/g)].map(m=>m[1]).concat(['speedPid','positionPid','headingPid','invert','heartbeatMs']));
for(const key of serverFields)assert(configKeys.includes(key)||run('compatibilityFields').includes(key),'未覆盖固件字段 '+key);
telemetry.fault='heartbeat_lost';telemetry.estop=false;run('ws.onmessage({data:JSON.stringify(testTelemetry)});start({t:"move",dist:10})');assert.equal(sent.at(-1).t,'move','失联历史故障不应永久阻止请求新动作');
const qr=context.qrcode(0,'M');qr.addData('http://192.168.4.1/');qr.make();assert(qr.getModuleCount()>20);assert(qr.isDark(0,0));
console.log('PASS: 双脚本语法 / 离线 QR / 无 CDN / 握手与遥测门控 / 控制权 / 精准心跳 / 失焦与 pointercancel 停车 / 急停 / 标定结果 / 配置字段');

// 键盘输入必须尊重编辑控件、当前页面和组合按键的逐个释放。
const keyEvent=(key,editing=false)=>({key,code:key===' '?'Space':'',repeat:false,target:{closest:()=>editing?{}:null},preventDefault(){}});
let count=sent.length;
windowEvents.keydown(keyEvent('w',true));windowEvents.keydown(keyEvent(' ',true));
assert.equal(sent.length,count,'设置编辑期间不得驱动或急停解锁');
run("workspaceMode='auto';");
windowEvents.keydown(keyEvent('w'));assert.equal(sent.length,count,'自动模式不接受手动驾驶键');
run("workspaceMode='manual';stop()");
windowEvents.keydown(keyEvent('w'));windowEvents.keydown(keyEvent('d'));windowEvents.keyup(keyEvent('d'));
assert.equal(sent.at(-1).t,'drv');assert.equal(sent.at(-1).x,0);assert.equal(sent.at(-1).y,1,'释放 D 后继续 W 前进');
windowEvents.keyup(keyEvent('w'));assert.equal(sent.at(-1).t,'stop');

// 保留历史字段的已有值，但不能生成可操作的假功能输入。
run('config={coastOnLoss:false};');assert.equal(run('collectConfig().coastOnLoss'),false);
run('config=null;');
assert(!elements.has('cfg_coastOnLoss'));
assert(html.includes('(-90 + (i * 360) / count)'),'雷达应保持前起顺时针等角排列');
assert(html.includes('ctx.setLineDash(valid ? [] : [4, 4])'),'无效测距必须以未知扇区而非空白表示');
assert(html.includes('id="workbench"')&&html.includes('id="landscapeToggle"'),'必须提供融合工作台与显式横屏开关');
assert.equal(run('hasConfig'),false,'本地默认草稿不能冒充已读取设备配置');
assert.equal(run('config'),null,'离线默认值不得进入设备配置快照');
assert.equal(elements.get('saveConfig').disabled,true,'没有设备回读配置时保存必须禁用');
const uiKeys=run('allFields.map(f=>f[0])'),defaultKeys=Object.keys(run('UI_DEFAULT_CONFIG'));
for(const key of uiKeys)assert(defaultKeys.includes(key),'离线默认配置缺字段 '+key);
for(const key of defaultKeys)assert(uiKeys.includes(key)||run('compatibilityFields').includes(key),'默认配置中有未展示字段 '+key);
assert.equal(run('collectConfig().maxSpeed'),100);assert.equal(run('collectConfig().developmentMode'),false);
assert.equal(elements.get('cfg_apSsid').value,'CombatBot-AP');assert.equal(elements.get('cfg_speedPid_kp').value,3);
run('globalThis.savedTm=tm;tm=null;drawRadar();');
assert(elements.get('irValues').textContent.includes('路测距未知'),'离线雷达仍展示未知通道');
run('tm=savedTm;');
assert(html.includes('id="calTurnLeft"')&&html.includes('id="calTurnRight"')&&html.includes('id="calClimbStart"'),'标定模式覆盖左右转和登台观测');

// 运行真实页面脚本：旁观者的生命周期事件不得产生全局 Stop。
const publish=(owner=0,st='idle')=>{telemetry.owner=owner;telemetry.st=st;telemetry.estop=false;run('ws.onmessage({data:JSON.stringify(testTelemetry)})')};
const lifecycle={blur:()=>windowEvents.blur(),visibility:()=>{context.document.hidden=true;documentEvents.visibilitychange();context.document.hidden=false},pagehide:()=>windowEvents.pagehide(),beforeunload:()=>windowEvents.beforeunload(),tab:()=>tabs[2].click()};
run('stop()');publish(99,'move');
count=sent.length;Object.values(lifecycle).forEach(fn=>fn());
assert.equal(sent.length,count,'旁观者失焦、隐藏、关闭或切页不能停止其他驾驶者');
publish();count=sent.length;Object.values(lifecycle).forEach(fn=>fn());
assert.equal(sent.length,count,'空闲浏览页面的生命周期也不能产生 Stop');
for(const [name,trigger] of Object.entries(lifecycle)){
  publish();run('start({t:"move",dist:30})');count=sent.length;trigger();
  assert.equal(sent.length,count+1,name+'：本页待受理精准动作离页必须停车');
  assert.equal(sent.at(-1).t,'stop');assert.equal(run('active'),false);
  Object.values(lifecycle).forEach(fn=>fn());assert.equal(sent.length,count+1,'同一离页过程不能重复发送 Stop');
}
publish();run('start({t:"turn",deg:90})');publish(7,'turn');count=sent.length;
lifecycle.blur();assert.equal(sent.length,count+1,'遥测确认主人身份后失焦仍必须停车');
lifecycle.pagehide();assert.equal(sent.length,count+1,'残留的本人 owner 遥测不能导致重复自动 Stop');
publish();run('drive(0,1)');publish(99,'drive');count=sent.length;lifecycle.blur();
assert.equal(sent.length,count,'被新 owner 接管后的旧本页动作不得自动停车');
elements.get('stop').click();assert.equal(sent.at(-1).t,'stop','手动 Stop 保持任何连接可执行');
elements.get('preciseStop').click();assert.equal(sent.at(-1).t,'stop');
elements.get('estop').click();assert.equal(sent.at(-1).t,'estop','旁观者仍可手动急停');

// 旧方向按钮释放与旧 Socket 回调不得停止重连后的新动作。
publish();directionButtons[0].onpointerdown({pointerId:11,preventDefault(){}});
run('globalThis.oldSocket=ws;connect()');
assert.equal(run('active'),false);assert.equal(run('tm'),null,'重连清除旧 owner 遥测');
count=sent.length;Object.values(lifecycle).forEach(fn=>fn());assert.equal(sent.length,count,'新连接握手前不得发送旧动作 Stop');
run('ws.onmessage({data:JSON.stringify({t:"hello",client:7,session:"1123456789abcdef0123456789abcdef"})})');
publish();run('start({t:"move",dist:15})');count=sent.length;
directionButtons[0].events.pointerup({pointerId:11});
run('oldSocket.onclose();oldSocket.onmessage({data:JSON.stringify({t:"tm",owner:99,st:"drive"})})');
assert.equal(sent.length,count,'旧输入释放或旧回调不能停止新连接动作');
assert.equal(run('active'),true);assert.equal(run('client'),7);
lifecycle.beforeunload();assert.equal(sent.at(-1).t,'stop','新连接当前主人离页仍应停车');
publish();directionButtons[0].onpointerdown({pointerId:12,preventDefault(){}});count=sent.length;
directionButtons[0].events.pointercancel({pointerId:12});assert.equal(sent.length,count+1,'当前方向按钮取消仍须停车');
console.log('PASS: spectator lifecycle / pending and confirmed owner Stop / repeated departure / takeover / manual Stop and estop / reconnect and stale pointer isolation');

// 验证请求凭据、服务端失败与断线前请求晚返回。
await Promise.resolve();await Promise.resolve();await Promise.resolve();
await run('api("/api/action",{t:"servo",pos:1})');
assert.equal(fetchCalls.at(-1).init.headers['X-CombatBot-Session'],run('sessionToken'));
assert.equal(JSON.parse(fetchCalls.at(-1).init.body).client,7);
context.fetch=async()=>({ok:false,status:400,json:async()=>({ok:false,message:'参数无效'})});
await assert.rejects(run('api("/api/config",{})'),/参数无效/);
let resolveOld;
context.fetch=()=>new Promise(resolve=>{resolveOld=resolve});
const revision=run('configRevision');
const oldRead=run('readConfig()');
run('ws.onclose()');
assert.equal(run('sessionToken'),'');assert.equal(run('hasConfig'),false);assert.equal(run('recentCal.odo'),false);
run('sessionToken="1123456789abcdef0123456789abcdef";client=8');
resolveOld({ok:true,json:async()=>({maxSpeed:999,defaultSpeed:999})});
await assert.rejects(oldRead,/旧会话响应已忽略/);
assert.equal(run('configRevision'),revision,'旧会话读取结果不能覆盖重连后表单');
run('sessionToken=""');
count=sent.length;run('drive(1,1)');assert.equal(sent.length,count,'没有随机凭据的连接不能驾驶');
await assert.rejects(run('api("/api/config")'),/未连接真实设备/);

// V5：离线输入只预览外观；横屏使用共同逆变换，不依赖系统方向。
count=sent.length;
run('tm=null;lastTm=0;');
elements.get('joystick').onpointerdown({pointerId:90,clientX:135,clientY:80,preventDefault(){}});
assert(elements.get('stick').style.transform.includes('calc'),'离线摇杆仍响应指针');
assert.equal(sent.length,count,'离线预览不能发送驱动');
assert(elements.get('driveState').textContent.includes('尚未发送'));
elements.get('joystick').events.pointerup({pointerId:90});
run('toggleLandscape()');
const point=run('eventToLocalPoint({clientX:190,clientY:135},$("joystick"))');
assert.equal(point.x,135);assert.equal(point.y,80);
run('toggleLandscape()');assert.equal(sent.length,count,'旋转展示不启动任务');

// V5：候选系数来自外测与设备脉冲，三个绑定字段缺一不可，失效记录不能保存。
context.confirmedConfig=run('({...UI_DEFAULT_CONFIG,revision:7})');
context.fetch=async(path,init)=>{fetchCalls.push({path,init});return {ok:true,json:async()=>path==='/api/config'?context.confirmedConfig:{ok:true,message:'saved'}}};
run('sessionToken="2123456789abcdef0123456789abcdef";client=8;fillConfig(confirmedConfig,true);');
telemetry.owner=0;telemetry.st='idle';telemetry.configRevision=7;telemetry.resultId=0;
run('ws.onmessage({data:JSON.stringify(testTelemetry)});calibration("turn_left");ws.onmessage({data:JSON.stringify({t:"done",type:"turn_left_cal",target:360,actual:360,err:0,id:10,sessionId:55,configRevision:7,calibrationTurnFactor:1.2})})');
assert.equal(run('recentCal.leftTurn'),true);
elements.get('measuredLeftTurn').value='300';elements.get('measuredLeftTurn').oninput();
assert(elements.get('calibrationPreview').textContent.includes('1.4400'),'预览应采用独立外测角度');
assert.equal(run('config.turnFactorLeft'),1,'预览不能改变设备确认系数');
await run('applyCalibration("turn_left","measuredLeftTurn")');
const applied=JSON.parse(fetchCalls.findLast(x=>x.path==='/api/calibrate').init.body);
assert.equal(applied.id,10);assert.equal(applied.sessionId,55);assert.equal(applied.configRevision,7);
assert.equal(applied.measured,300);assert.equal(run('recentCal.leftTurn'),false,'已应用结果不能重复复用');
run('calibration("turn_right");ws.onmessage({data:JSON.stringify({t:"done",type:"turn_right_cal",target:-360,actual:-360,err:0,id:11,sessionId:55,configRevision:7,calibrationTurnFactor:1})})');
assert.equal(run('recentCal.rightTurn'),true);
telemetry.configRevision=8;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');
assert.equal(run('recentCal.rightTurn'),false,'设备参数变化后旧标定结果必须失效');
telemetry.configRevision=7;run('ws.onmessage({data:JSON.stringify(testTelemetry)});calibration("odo");ws.onmessage({data:JSON.stringify({t:"done",type:"odo_cal",target:100,actual:100,id:12})})');
assert.equal(run('recentCal.odo'),false,'缺少结果绑定字段不能授权保存');
run('calibration("climb_observe");');elements.get('calClimbEnd').click();
assert.equal(sent.at(-1).mode,'climb_observe_end');
run('ws.onmessage({data:JSON.stringify({t:"done",type:"climb_observe",id:13,sessionId:55,configRevision:7,actual:23,calibrationDurationMs:3000,calibrationMaxTiltDeg:12,calibrationSupportChanges:2})})');
assert(elements.get('climbObservationResult').textContent.includes('轮程估计'));
assert.equal(run('Object.keys(recentCal.records).length'),0,'登台观测不能被当作几何修正');
console.log('PASS: V5 offline joystick / explicit rotation inverse / measured coefficient preview / bound apply / revision invalidation / incomplete result refusal / read-only climb observation');

// 开发超时必须撤销持续输入；按住键盘重复事件、计时续传都不能自行恢复。
tabs[0].click();telemetry.fault='';telemetry.owner=0;telemetry.st='idle';publish();
windowEvents.keydown(keyEvent('w'));assert.equal(sent.at(-1).t,'drv');
telemetry.fault='development_drive_timeout';telemetry.owner=0;telemetry.st='idle';publish();
assert.equal(run('active'),false);assert.equal(run('continuous'),false);assert.equal(run('pressed.size'),0);
count=sent.length;intervals[0]();intervals[0]();
const repeating=keyEvent('w');repeating.repeat=true;windowEvents.keydown(repeating);
assert.equal(sent.length,count,'开发超时后的周期和重复按键不得恢复非零驱动');
windowEvents.keyup(keyEvent('w'));assert.equal(sent.at(-1).t,'stop','松开超时按键须明确Stop重新准入');
assert.equal(run('manualTimeoutPending'),false);
telemetry.fault='';publish();windowEvents.keydown(keyEvent('w'));assert.equal(sent.at(-1).t,'drv');
windowEvents.keyup(keyEvent('w'));
elements.get('joystick').onpointerdown({pointerId:94,clientX:135,clientY:80,preventDefault(){}});
telemetry.fault='development_drive_timeout';publish();count=sent.length;
intervals[0]();assert.equal(sent.length,count);assert.equal(run('pointer'),null);
elements.get('joystick').events.pointerup({pointerId:94});assert.equal(sent.at(-1).t,'stop','超时指针松手仍补发Stop');

// 云端收到相同车端超时也必须清输入；控制租约心跳可留存，非零drv不能续传。
telemetry.fault='';publish();
run('cloudMode=true;cloudAuthorized=true;cloudDeviceId="one";cloudLease="lease";cloudBoot="boot";cloudLink="link";cloudPermit="permit";cloudDeviceState={online:true};drive(0,1);');
assert.equal(sent.at(-1).command.t,'drv');
telemetry.fault='development_drive_timeout';run('cloudMessage(testTelemetry)');
assert.equal(run('active'),false);assert.equal(run('continuous'),false);
count=sent.length;intervals[0]();intervals[0]();
assert(!sent.slice(count).some(m=>m.command?.t==='drv'),'云端超时后不能定时恢复驾驶');
elements.get('stop').click();assert.equal(sent.at(-1).command.t,'stop','云端明确停止可确认重新准入');
assert.equal(run('manualTimeoutPending'),false);
console.log('PASS: development timeout clears AP/cloud manual input / keyboard repeat suppression / no periodic nonzero drv / pointer and keyboard release explicit Stop / cloud Stop rearm');
console.log('PASS: session headers / pre-telemetry config / HTTP failures / disconnect invalidation / stale responses / keyboard lifecycle / compatible legacy fields / IR mapping');
