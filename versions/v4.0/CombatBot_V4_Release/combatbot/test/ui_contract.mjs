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
  addEventListener(type,fn){this.events[type]=fn} setAttribute(){} add(option){this.children.push(option)}
  getContext(){return canvasContext} getBoundingClientRect(){return {left:0,top:0,width:270,height:270}}
  setPointerCapture(){} reportValidity(){return true} click(){this.onclick?.({target:this})} closest(){return null}
}
for(const m of html.matchAll(/<([\w]+)([^>]*\bid="([^"]+)"[^>]*)>/g)){const el=new Element(m[1]);el.id=m[3];el.width=520;el.height=400;const value=m[2].match(/\bvalue="([^"]*)"/);if(value)el.value=value[1];const className=m[2].match(/\bclass="([^"]*)"/);if(className){el.className=className[1];className[1].split(' ').forEach(c=>el.classList.add(c))}}
const tabs=['continuous','precise','sensors','settings'].map(name=>{const el=new Element('button');el.dataset.tab=name;return el});
const directionButtons=[...html.matchAll(/data-dir="([^"]+)"/g)].map(match=>{const el=new Element('button');el.dataset.dir=match[1];return el});
const motion=[...elements.values()].filter(e=>e.classList.contains('motion'));
const sent=[],fetchCalls=[];class WS {static OPEN=1;constructor(url){this.url=url;this.readyState=1}send(value){sent.push(JSON.parse(value))}}
const context={console,Date,Math,Number,Array,Object,JSON,Error,TextEncoder,Blob,URL,URLSearchParams,Option:function(label,value){this.label=label;this.value=value},WebSocket:WS,
  location:{protocol:fileMode?'file:':'http:',host:'192.168.4.1',hostname:'192.168.4.1',origin:'http://192.168.4.1'},
  document:{hidden:false,getElementById:id=>elements.get(id),createElement:tag=>new Element(tag),createTextNode:text=>({textContent:text}),querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='[data-dir]'?directionButtons:selector==='.tab'?['continuous','precise','sensors','settings'].map(id=>elements.get(id)):selector==='.motion'?motion:[],addEventListener:(type,fn)=>documentEvents[type]=fn},
  window:{addEventListener:(type,fn)=>windowEvents[type]=fn},setInterval:fn=>intervals.push(fn),setTimeout:()=>1,clearTimeout:()=>{},confirm:()=>true,
  fetch:async(path,init)=>{fetchCalls.push({path,init});return {ok:true,json:async()=>({ok:true})}}};
vm.createContext(context);blocks.forEach((b,i)=>vm.runInContext(b,context,{filename:'web-script-'+i+'.js'}));
const run=code=>vm.runInContext(code,context);
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
const telemetry={t:'tm',owner:0,st:'idle',fault:'',estop:false,progress:0,rpm:[0,0,0,0],spd:0,yaw:0,odo:0,bat:24,acc:[0,0,1],gr:[false,true,false,false],e18:[false,false,false],ir:[50,70,90,110,130,140],ioOk:true,accelOk:true,net:'AP_ONLY'};
context.testTelemetry=telemetry;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');
run('drive(.5,.8)');assert.equal(sent.at(-1).t,'drv');assert.equal(sent.at(-1).x,.5,'死区/expo 不应在网页重复施加');assert.equal(sent.at(-1).y,.8);
intervals[0]();assert(sent.some(m=>m.t==='hb'),'运动期间必须每 200 ms 发心跳');
windowEvents.blur();assert.equal(sent.at(-1).t,'stop','失焦立即请求停车');
run('start({t:"move",dist:30})');assert.equal(sent.at(-1).t,'move');intervals[0]();assert.equal(sent.at(-1).t,'hb','精准动作也必须保持心跳');
telemetry.owner=99;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');const before=sent.length;run('drive(1,1)');assert.equal(sent.length,before,'其他客户端持有动作时禁止发驱动');
telemetry.owner=0;run('ws.onmessage({data:JSON.stringify(testTelemetry)})');elements.get('joystick').onpointerdown({pointerId:42,clientX:135,clientY:80,preventDefault(){}});assert.equal(sent.at(-1).t,'drv');elements.get('joystick').events.pointercancel({pointerId:42});assert.equal(sent.at(-1).t,'stop','pointercancel 必须停车');
elements.get('estop').click();assert.equal(sent.at(-1).t,'estop');
run('ws.onmessage({data:JSON.stringify({t:"done",type:"odo_cal",target:100,actual:99.5,err:-.5,id:2})})');assert.equal(run('recentCal.odo'),false,'其他客户端标定结果只展示，不能授权本页应用');
run('calibration("odo");ws.onmessage({data:JSON.stringify({t:"done",type:"odo_cal",target:100,actual:99.5,err:-.5,id:3})})');assert.equal(run('recentCal.odo'),true);
run('calibration("turn");ws.onmessage({data:JSON.stringify({t:"done",type:"turn_cal",target:360,actual:359,err:-1,id:4})})');assert.equal(run('recentCal.turn'),true);
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
run("$('continuous').classList.remove('active')");
windowEvents.keydown(keyEvent('w'));assert.equal(sent.length,count,'其他页不接受驾驶键');
run("$('continuous').classList.add('active');stop()");
windowEvents.keydown(keyEvent('w'));windowEvents.keydown(keyEvent('d'));windowEvents.keyup(keyEvent('d'));
assert.equal(sent.at(-1).t,'drv');assert.equal(sent.at(-1).x,0);assert.equal(sent.at(-1).y,1,'释放 D 后继续 W 前进');
windowEvents.keyup(keyEvent('w'));assert.equal(sent.at(-1).t,'stop');

// 保留历史字段的已有值，但不能生成可操作的假功能输入。
run('config={coastOnLoss:false};');assert.equal(run('collectConfig().coastOnLoss'),false);
assert(!elements.has('cfg_coastOnLoss'));
assert(html.includes('(-90 + (i * 360) / ir.length)'),'雷达应与固件前起顺时针等角排列一致');

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
console.log('PASS: session headers / pre-telemetry config / HTTP failures / disconnect invalidation / stale responses / keyboard lifecycle / compatible legacy fields / IR mapping');
