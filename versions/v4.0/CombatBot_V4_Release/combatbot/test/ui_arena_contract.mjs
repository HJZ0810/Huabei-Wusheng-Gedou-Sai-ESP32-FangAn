// V4 竞技地图与双传输合同：node test/ui_arena_contract.mjs
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
for(const m of html.matchAll(/<([\w]+)([^>]*\bid="([^"]+)"[^>]*)>/g)){const el=new Element(m[1]);el.id=m[3];el.width=Number(m[2].match(/width="(\d+)"/)?.[1]||520);el.height=Number(m[2].match(/height="(\d+)"/)?.[1]||400);const value=m[2].match(/\bvalue="([^"]*)"/);if(value)el.value=value[1];const className=m[2].match(/\bclass="([^"]*)"/);if(className){el.className=className[1];className[1].split(' ').forEach(c=>el.classList.add(c))}}
const tabs=['continuous','arena','precise','sensors','settings'].map(name=>{const el=new Element('button');el.dataset.tab=name;return el});
const directionButtons=[...html.matchAll(/data-dir="([^"]+)"/g)].map(match=>{const el=new Element('button');el.dataset.dir=match[1];return el});
const motion=[...elements.values()].filter(e=>e.classList.contains('motion'));
const sent=[],fetchCalls=[];class WS {static OPEN=1;constructor(url){this.url=url;this.readyState=1}send(value){sent.push(JSON.parse(value))}close(){this.readyState=3;this.onclose?.()}}
const context={console,Date,Math,Number,Array,Object,JSON,Error,TextEncoder,Blob,URL,URLSearchParams,Option:function(label,value){this.label=label;this.value=value},WebSocket:WS,
  location:{protocol:fileMode?'file:':'http:',host:'192.168.4.1',hostname:'192.168.4.1',origin:'http://192.168.4.1',pathname:'/',search:'',hash:''},
  history:{replaceState:(state,title,url)=>{context.location.hash=url.includes('#')?'#'+url.split('#')[1]:'';context.lastHistoryUrl=url}},
  document:{hidden:false,getElementById:id=>elements.get(id),createElement:tag=>new Element(tag),createTextNode:text=>({textContent:text}),querySelectorAll:selector=>selector==='[data-tab]'?tabs:selector==='[data-dir]'?directionButtons:selector==='.tab'?['continuous','arena','precise','sensors','settings'].map(id=>elements.get(id)):selector==='.motion'?motion:[],addEventListener:(type,fn)=>documentEvents[type]=fn},
  window:{addEventListener:(type,fn)=>windowEvents[type]=fn},setInterval:fn=>intervals.push(fn),setTimeout:()=>1,clearTimeout:()=>{},confirm:()=>true,
  fetch:async(path,init)=>{fetchCalls.push({path,init});return {ok:true,json:async()=>({ok:true})}}};
vm.createContext(context);blocks.forEach((b,i)=>vm.runInContext(b,context,{filename:'web-script-'+i+'.js'}));
const run=code=>vm.runInContext(code,context);

// 先完成异步探测：普通 AP 的非云端响应不得改变原有连接。
await Promise.resolve();await Promise.resolve();await Promise.resolve();await Promise.resolve();
assert.equal(run('cloudMode'),false);
assert(fetchCalls.some(call=>call.path==='./api/info'));
run('ws.onmessage({data:JSON.stringify({t:"hello",client:7,session:"0123456789abcdef0123456789abcdef"})})');
const telemetry={t:'tm',owner:0,st:'idle',estop:false,rpm:[0,0,0,0],gr:[false,false,false,false],e18:[false,false,false],ir:[40,70,90,110,130,140],acc:[0,0,1],ioOk:true,accelOk:true,arena:{x:20,y:-30,heading:90,uncertainty:12,quality:.7,floor:'lower',mode:'idle',phase:'idle',reason:'',active:false,outerSize:380,platformSize:240,entryX:0,entryY:-155}};
context.testTelemetry=telemetry;
const publish=()=>run('ws.onmessage({data:JSON.stringify(testTelemetry)})');publish();
assert.equal(elements.get('arenaPosition').textContent,'20.0 / -30.0');
assert.equal(elements.get('arenaQuality').textContent,'70%');
assert(elements.get('arenaMode').textContent.includes('低层'));
assert.equal(run('arenaTrail.length'),1);
let count=sent.length;
elements.get('arenaMap').onclick({clientX:135,clientY:135});
assert.equal(sent.length,count,'地图选点不能直接启动电机');
assert.equal(run('selectedTarget.x'),0);assert.equal(run('selectedTarget.y'),0);
elements.get('arenaMap').onkeydown({key:'ArrowRight',shiftKey:false,preventDefault(){}});
assert.equal(run('selectedTarget.x'),5);assert.equal(sent.length,count,'键盘选点也必须等待显式执行');
elements.get('arenaGoto').click();assert.equal(sent.at(-1).t,'goto');assert.equal(sent.at(-1).x,5);
assert.equal(run('active'),false,'自主任务不使用人工心跳状态');
count=sent.length;
windowEvents.blur();windowEvents.pagehide();windowEvents.beforeunload();context.document.hidden=true;documentEvents.visibilitychange();context.document.hidden=false;tabs[2].click();
assert.equal(sent.length,count,'自治请求受理前离页不得取消自治');
telemetry.arena.active=true;telemetry.arena.mode='navigate';telemetry.arena.phase='approach';publish();
windowEvents.blur();intervals[0]();assert.equal(sent.length,count,'自治受理后不能按人工离页和心跳逻辑停车');
assert.equal(elements.get('arenaGoto').disabled,true);
elements.get('arenaTakeover').click();assert.equal(sent.at(-1).t,'takeover');
telemetry.arena.active=false;telemetry.arena.mode='idle';run('autonomousRequestedAt=0');publish();
run('drive(0,1)');assert.equal(sent.at(-1).t,'drv');windowEvents.blur();assert.equal(sent.at(-1).t,'stop','接管后的人工输入仍需离页停车');
run('stop()');elements.get('arenaAuto').click();assert.equal(sent.at(-1).t,'auto');elements.get('arenaStop').click();assert.equal(sent.at(-1).t,'stop','显式停止必须取消全部任务');
elements.get('arenaClimb').click();assert.equal(sent.at(-1).t,'climb');elements.get('arenaStop').click();
elements.get('poseX').value='-30';elements.get('poseY').value='-160';elements.get('poseHeading').value='90';elements.get('poseFloor').value='lower';
elements.get('poseForm').onsubmit({preventDefault(){}});assert.equal(sent.at(-1).t,'pose');assert.equal(sent.at(-1).floor,'lower');assert.equal(sent.at(-1).x,-30);
count=sent.length;elements.get('poseX').value='NaN';elements.get('poseForm').onsubmit({preventDefault(){}});assert.equal(sent.length,count,'非有限起点不得发送');
assert.equal(run('selectArenaTarget(191,0)'),false,'围栏外目标不自动夹到边界');
telemetry.arena.x=225;telemetry.arena.floor='upper_estimated';publish();assert.equal(elements.get('arenaPosition').textContent,'225.0 / -30.0','估计漂出场外必须如实显示');assert(elements.get('arenaMode').textContent.includes('仅推定'));
const fields=run('allFields.map(f=>f[0])');for(const key of ['arenaClimbEnabled','arenaCalibrated','arenaEntryHeading','cloudEnabled','cloudCaPem','cloudDeviceKey'])assert(fields.includes(key));
run('fillConfig({maxSpeed:100,defaultSpeed:40,cloudDeviceKey:"never-display",cloudCaPem:"CERT",arenaOuterCm:380,arenaPlatformCm:240})');
assert.equal(elements.get('cfg_cloudDeviceKey').value,'');assert.equal(run('config.cloudDeviceKey'),undefined);
assert.equal(run('collectConfig().cloudDeviceKey'),undefined,'未编辑设备密钥不能把遮罩或空字符串写回设备');
elements.get('cfg_cloudDeviceKey').value='';elements.get('cfg_cloudDeviceKey').oninput();assert.equal(run('collectConfig().cloudDeviceKey'),'','明确编辑后空字符串表示清除');
assert.equal(elements.get('cfg_cloudCaPem').tagName,'TEXTAREA');
console.log('PASS: AP / map selection / explicit navigation / autonomous lifecycle / takeover / manual loss-stop / pose / unclamped estimate / credential edit semantics');

// 单车入口：专用片段链接换 Cookie；页面没有账号、车辆选择或自动抢占。
assert(!/id="cloudLogin"|id="cloudUsername"|id="cloudPassword"|id="cloudDevice"/.test(html));
const device={deviceId:'bot-one',online:true,boot:'boot-a',link:'link-a',permit:'permit-a',permitTtlMs:600,owner:null};
context.cloudDevice=device;
context.location.hash='#control=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
context.fetch=async(path,init)=>{
  fetchCalls.push({path,init});
  assert.equal(context.location.hash,'','专用链接密钥必须在请求前从地址栏移除');
  return {ok:true,json:async()=>path==='./api/info'?{mode:'cloud',singleDevice:true,deviceId:'bot-one',basePath:'',wsOperator:'/ws/operator'}:{ok:true,device}};
};
await run('discoverCloud()');
assert.equal(run('cloudMode'),true);assert.equal(run('ws.url'),'ws://192.168.4.1/ws/operator');
assert.equal(context.lastHistoryUrl,'/');
assert(fetchCalls.some(x=>x.path==='/api/connect'&&JSON.parse(x.init.body).controlKey==='aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'));
count=sent.length;
run('ws.onmessage({data:JSON.stringify({t:"welcome",device:cloudDevice})})');
assert.equal(sent.length,count,'唯一车辆由服务端绑定，不再发送选车或自动抢占请求');
assert.equal(run('cloudDeviceId'),'bot-one');assert.equal(run('cloudAuthorized'),true);
telemetry.arena.active=false;telemetry.arena.x=20;publish();count=sent.length;
run('drive(0,1)');assert.equal(sent.length,count,'未明确接管不能驱动');
elements.get('cloudClaim').click();assert.equal(sent.at(-1).t,'claim');assert.equal(sent.at(-1).deviceId,'bot-one');
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"lease-one",boot:"boot-a",link:"link-a",permit:"permit-a"})})');
run('drive(0,1)');let packet=sent.at(-1);assert.equal(packet.t,'command');assert.equal(packet.command.t,'drv');assert.equal(packet.lease,'lease-one');assert.equal(packet.permit,'permit-a');assert.equal(packet.seq,1);
run('ws.onmessage({data:JSON.stringify({t:"permit",deviceId:"bot-one",boot:"boot-a",link:"link-a",permit:"permit-b"})})');
intervals[0]();const hb=sent.findLast(x=>x.t==='command'&&x.command.t==='hb');assert(hb);assert.equal(hb.permit,'permit-b');assert(hb.seq>packet.seq);
count=sent.length;run('send({t:"unlock"})');assert.equal(sent.length,count,'远端急停解锁不能进入命令通道');
await assert.rejects(run('api("/api/config")'),/云端不开放/);
elements.get('cloudRelease').click();assert.equal(sent.at(-1).t,'release');assert.equal(run('cloudLease'),'');
// 空闲控制权也必须由真实页面心跳维持，不让服务器代替操作者续命。
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"lease-two",permit:"permit-c"})})');count=sent.length;intervals[0]();assert.equal(sent.length,count+1);assert.equal(sent.at(-1).command.t,'hb');
assert.equal(run('bindCloudDevice({deviceId:"other-bot",online:true})'),false,'不能切换到配置外的车辆');
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"other-bot",lease:"foreign",permit:"bad"})})');assert.equal(run('cloudLease'),'lease-two');
run('globalThis.oldCloud=ws;connect()');assert.equal(run('active'),false);assert.equal(run('cloudLease'),'');assert.equal(run('tm'),null,'重连清遥测和人工动作，不自动恢复控制');
count=sent.length;run('oldCloud.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"stale",permit:"old"})})');assert.equal(run('cloudLease'),'');assert.equal(sent.length,count,'旧连接回调不能恢复旧控制权');
run('ws.onmessage({data:JSON.stringify({t:"welcome",device:cloudDevice})})');assert.equal(sent.length,count,'重连只恢复唯一车辆的遥测');assert.equal(run('cloudLease'),'');publish();
// 撤销控制权、设备换会话、过期许可都不能恢复人工输入。
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"epoch-lease",boot:"boot-a",link:"link-a",permit:"p-a"})})');
run('ws.onmessage({data:JSON.stringify({t:"permit",deviceId:"bot-one",boot:"boot-b",link:"link-b",permit:"p-b"})})');
assert.equal(run('cloudLease'),'','boot/link 变化必须撤销旧控制权');
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"new-lease",boot:"boot-b",link:"link-b",permit:"p-b"})})');
run('ws.onmessage({data:JSON.stringify({t:"lease_end",deviceId:"bot-one",lease:"epoch-lease",reason:"operator_heartbeat_lost"})})');assert.equal(run('cloudLease'),'new-lease','旧 lease_end 不得撤销新控制权');
run('ws.onmessage({data:JSON.stringify({t:"lease_end",deviceId:"bot-one",lease:"new-lease",reason:"operator_heartbeat_lost"})})');assert.equal(run('cloudLease'),'');
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"expiry-lease",permit:"p-c"})})');
run('ws.onmessage({data:JSON.stringify({t:"error",code:"permit_expired",message:"expired"})})');assert.equal(run('cloudPermit'),'');assert.equal(run('active'),false);
run('ws.onmessage({data:JSON.stringify({t:"device",deviceId:"bot-one",online:false,permit:""})})');assert.equal(run('cloudLease'),'');assert.equal(run('lastTm'),0);assert.equal(elements.get('cloudClaim').disabled,true);
assert(!JSON.stringify(sent).includes('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'),'链接密钥不能出现在设备指令');
assert(!elements.get('events').children.some(e=>e.textContent.includes('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa')),'链接密钥不能写入事件记录');
// 已有 Cookie 的普通刷新只请求 me；无会话的公开入口给出短提示并禁用控制。
context.fetch=async(path,init)=>{fetchCalls.push({path,init});return {ok:true,json:async()=>path==='./api/info'?{mode:'cloud',singleDevice:true,deviceId:'bot-one',basePath:'',wsOperator:'/ws/operator'}:{ok:true,device}}};
count=fetchCalls.length;await run('discoverCloud()');assert(fetchCalls.slice(count).some(x=>x.path==='/api/me'));assert(!fetchCalls.slice(count).some(x=>x.path==='/api/connect'));
context.fetch=async(path)=>({ok:path==='./api/info',status:401,json:async()=>path==='./api/info'?{mode:'cloud',singleDevice:true,deviceId:'bot-one',basePath:'',wsOperator:'/ws/operator'}:{ok:false,message:'unauthorized'}});
await run('discoverCloud()');assert.equal(run('cloudAuthorized'),false);assert.equal(elements.get('cloudHint').textContent,'请使用专用控制链接');assert.equal(elements.get('cloudClaim').disabled,true);
count=sent.length;elements.get('cloudClaim').click();run('drive(0,1)');assert.equal(sent.length,count,'无会话入口不能发起控制');
console.log('PASS: single vehicle / capability fragment removal / HttpOnly session exchange / explicit claim / lease seq permit / heartbeat / sole device binding / local-only admin / release / reconnect / stale socket / unauthorized entry');

// 已打开公开页面后，浏览器只改变片段，也必须立即消费链接并恢复连接。
context.fetch=async(path,init)=>{fetchCalls.push({path,init});assert.equal(context.location.hash,'');return {ok:true,json:async()=>({ok:true,device})}};
context.location.hash='#control='+'b'.repeat(64);count=sent.length;
const reentry=windowEvents.hashchange();assert.equal(context.location.hash,'','hashchange 回调必须同步移除密钥');
await reentry;assert.equal(run('cloudAuthorized'),true);assert.equal(run('cloudLease'),'');assert.equal(sent.length,count,'同页授权也不能自动接管或发送动作');
assert.equal(run('ws.url'),'ws://192.168.4.1/ws/operator');
run('ws.onmessage({data:JSON.stringify({t:"welcome",device:cloudDevice})})');publish();
run('ws.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"old-page-lease",boot:"boot-a",link:"link-a",permit:"permit-a"})});drive(0,1);globalThis.replacedSocket=ws');
count=sent.length;context.location.hash='#control='+'c'.repeat(64);await windowEvents.hashchange();
assert.equal(sent.length,count+1);assert.equal(sent.at(-1).t,'release','替换链接前只释放旧控制权，不发全局停车中断自治');
assert.equal(run('replacedSocket.readyState'),3);assert.equal(run('cloudLease'),'');assert.equal(run('active'),false);assert.equal(run('continuous'),false);
count=sent.length;run('replacedSocket.onmessage({data:JSON.stringify({t:"lease",deviceId:"bot-one",lease:"stale-reentry",permit:"old"})})');assert.equal(run('cloudLease'),'');assert.equal(sent.length,count);
// 畸形与重复密钥不能被既有 Cookie 自动授权，也不进入网络或日志。
for(const hash of ['#control=not-a-key','#control=','#control='+'d'.repeat(64)+'&control='+'e'.repeat(64)]){
  context.location.hash=hash;count=fetchCalls.length;await windowEvents.hashchange();
  assert.equal(context.location.hash,'');assert.equal(run('cloudAuthorized'),false);assert.equal(run('ws'),null);assert.equal(fetchCalls.length,count,'畸形专用链接不能发起 connect/me');
}
// 较早的授权请求迟到，不能覆盖随后入口拒绝后的页面状态。
let resolvePending;
context.fetch=async(path,init)=>{fetchCalls.push({path,init});return new Promise(resolve=>{resolvePending=resolve})};
context.location.hash='#control='+'f'.repeat(64);const pendingEntry=windowEvents.hashchange();
context.location.hash='#control=malformed';await windowEvents.hashchange();
resolvePending({ok:true,json:async()=>({ok:true,device})});await pendingEntry;
assert.equal(run('cloudAuthorized'),false);assert.equal(run('ws'),null);assert.equal(run('cloudLease'),'');
console.log('PASS: same-page control link / synchronous fragment removal / prior lease release / stale socket isolation / malformed and duplicate key rejection / stale HTTP response isolation');
